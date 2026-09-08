#include "XRRuntimeSelection.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace Modularity::XR {

namespace {

std::string g_activeOverride;
bool g_runtimeLoaded = false;

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

fs::path pathFromUtf8(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string pathToUtf8(const fs::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(pathFromUtf8(path), ec);
}

#if defined(_WIN32)

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                          result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                          result.data(), size);
    return result;
}

constexpr REGSAM kRegAccess = KEY_READ | KEY_WOW64_64KEY;
constexpr const wchar_t* kOpenXRKey = L"SOFTWARE\\Khronos\\OpenXR\\1";
constexpr const wchar_t* kAvailableKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes";

bool readRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                        std::string& out) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, kRegAccess, &key) != ERROR_SUCCESS) return false;

    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = ::RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes == 0 ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        ::RegCloseKey(key);
        return false;
    }

    std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    status = ::RegQueryValueExW(key, valueName, nullptr, &type,
                                reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS) return false;

    buffer.resize(::wcsnlen(buffer.c_str(), buffer.size()));
    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[MAX_PATH * 2] = {};
        const DWORD written = ::ExpandEnvironmentStringsW(buffer.c_str(), expanded,
                                                          static_cast<DWORD>(std::size(expanded)));
        if (written > 0 && written <= std::size(expanded)) buffer.assign(expanded);
    }
    out = wideToUtf8(buffer);
    return !out.empty();
}

void enumerateAvailableRuntimes(HKEY root, std::vector<std::string>& out) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, kAvailableKey, 0, kRegAccess, &key) != ERROR_SUCCESS) return;

    for (DWORD index = 0;; ++index) {
        wchar_t nameBuffer[1024] = {};
        DWORD nameLength = static_cast<DWORD>(std::size(nameBuffer));
        DWORD type = 0;
        DWORD data = 0;
        DWORD dataSize = sizeof(data);
        const LSTATUS status =
            ::RegEnumValueW(key, index, nameBuffer, &nameLength, nullptr, &type,
                            reinterpret_cast<LPBYTE>(&data), &dataSize);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        if (nameLength == 0) continue;
        if (type == REG_DWORD && data != 0) continue;

        std::string path = wideToUtf8(std::wstring(nameBuffer, nameLength));
        if (!path.empty()) out.push_back(std::move(path));
    }
    ::RegCloseKey(key);
}

#endif // _WIN32

std::string extractJsonString(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = text.find(needle);
    if (at == std::string::npos) return {};
    at = text.find(':', at + needle.size());
    if (at == std::string::npos) return {};
    at = text.find('"', at + 1);
    if (at == std::string::npos) return {};
    ++at;

    std::string value;
    for (size_t i = at; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            value += (next == 'n') ? '\n' : next;
            ++i;
            continue;
        }
        if (c == '"') break;
        value += c;
    }
    return value;
}

bool readManifest(const std::string& manifestPath, std::string& outName,
                  std::string& outLibrary) {
    std::ifstream file(pathFromUtf8(manifestPath), std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    if (text.empty() || text.size() > 64 * 1024) return false;
    outName = extractJsonString(text, "name");
    outLibrary = extractJsonString(text, "library_path");
    return true;
}

std::string vendorNameFromPath(const std::string& manifestPath) {
    const std::string lowered = toLower(manifestPath);
    const auto has = [&lowered](const char* needle) {
        return lowered.find(needle) != std::string::npos;
    };
    if (has("steamvr") || has("steamxr")) return "SteamVR";
    if (has("oculus")) return "Oculus / Meta Quest Link";
    if (has("virtualdesktop") || has("virtual desktop")) return "Virtual Desktop";
    if (has("mixedrealityruntime") || has("windowsmr") || has("windows mixed reality")) {
        return "Windows Mixed Reality";
    }
    if (has("varjo")) return "Varjo";
    if (has("vive")) return "VIVE OpenXR";
    if (has("pimax")) return "Pimax";
    if (has("monado")) return "Monado";
    if (has("wivrn")) return "WiVRn";
    if (has("alvr")) return "ALVR";
    if (has("openxr_runtime_debug") || has("api_layer")) return "OpenXR (debug)";
    return {};
}

std::string folderNameFromPath(const std::string& manifestPath) {
    const fs::path path = pathFromUtf8(manifestPath);
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        const std::string name = pathToUtf8(parent.filename());
        if (!name.empty()) return name;
    }
    const std::string stem = pathToUtf8(path.stem());
    return stem.empty() ? manifestPath : stem;
}

void addCandidate(std::vector<std::string>& list, std::string path) {
    if (path.empty()) return;
    std::error_code ec;
    const fs::path normalized = fs::weakly_canonical(pathFromUtf8(path), ec);
    if (!ec && !normalized.empty()) path = pathToUtf8(normalized);
    const std::string key = toLower(path);
    for (const std::string& existing : list) {
        if (toLower(existing) == key) return;
    }
    list.push_back(std::move(path));
}

void addWellKnownCandidates(std::vector<std::string>& list) {
#if defined(_WIN32)
    static const char* const kWellKnown[] = {
        "C:/Program Files (x86)/Steam/steamapps/common/SteamVR/steamxr_win64.json",
        "C:/Program Files/Oculus/Support/oculus-runtime/oculus_openxr_64.json",
    };
    for (const char* candidate : kWellKnown) {
        if (fileExists(candidate)) addCandidate(list, candidate);
    }
#else
    static const char* const kWellKnown[] = {
        "/usr/share/openxr/1/openxr_monado.json",
        "/usr/local/share/openxr/1/openxr_monado.json",
    };
    for (const char* candidate : kWellKnown) {
        if (fileExists(candidate)) addCandidate(list, candidate);
    }
#endif
}

std::string environmentValue(const char* name) {
#if defined(_WIN32)
    const std::wstring wideName = utf8ToWide(name);
    wchar_t buffer[4096] = {};
    const DWORD written =
        ::GetEnvironmentVariableW(wideName.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) return {};
    return wideToUtf8(std::wstring(buffer, written));
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

bool setEnvironmentValue(const char* name, const std::string& value) {
#if defined(_WIN32)
    const std::wstring wideName = utf8ToWide(name);
    const std::wstring wideValue = utf8ToWide(value);
    const bool win32Ok =
        ::SetEnvironmentVariableW(wideName.c_str(), value.empty() ? nullptr : wideValue.c_str()) != 0;
    const bool crtOk = ::_wputenv_s(wideName.c_str(), wideValue.c_str()) == 0;
    return win32Ok || crtOk;
#else
    if (value.empty()) return ::unsetenv(name) == 0;
    return ::setenv(name, value.c_str(), 1) == 0;
#endif
}

} // namespace

std::string SystemDefaultRuntimeManifestPath() {
#if defined(_WIN32)
    std::string path;
    if (readRegistryString(HKEY_CURRENT_USER, kOpenXRKey, L"ActiveRuntime", path)) return path;
    if (readRegistryString(HKEY_LOCAL_MACHINE, kOpenXRKey, L"ActiveRuntime", path)) return path;
    return {};
#else
    std::vector<std::string> candidates;
    const std::string configHome = environmentValue("XDG_CONFIG_HOME");
    if (!configHome.empty()) candidates.push_back(configHome + "/openxr/1/active_runtime.json");
    const std::string home = environmentValue("HOME");
    if (!home.empty()) candidates.push_back(home + "/.config/openxr/1/active_runtime.json");
    candidates.push_back("/etc/xdg/openxr/1/active_runtime.json");
    candidates.push_back("/usr/local/share/openxr/1/active_runtime.json");
    candidates.push_back("/usr/share/openxr/1/active_runtime.json");
    for (const std::string& candidate : candidates) {
        if (fileExists(candidate)) return candidate;
    }
    return {};
#endif
}

std::string DescribeRuntimeManifest(const std::string& manifestPath) {
    if (manifestPath.empty()) return {};
    std::string name;
    std::string library;
    if (readManifest(manifestPath, name, library) && !name.empty()) return name;
    const std::string vendor = vendorNameFromPath(manifestPath);
    if (!vendor.empty()) return vendor;
    return folderNameFromPath(manifestPath);
}

std::vector<XRInstalledRuntime> EnumerateInstalledRuntimes() {
    const std::string systemDefault = SystemDefaultRuntimeManifestPath();

    std::vector<std::string> paths;
    addCandidate(paths, systemDefault);
#if defined(_WIN32)
    enumerateAvailableRuntimes(HKEY_CURRENT_USER, paths);
    enumerateAvailableRuntimes(HKEY_LOCAL_MACHINE, paths);
#else
    for (const char* dir : { "/usr/share/openxr/1", "/usr/local/share/openxr/1" }) {
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(dir), ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".json") continue;
            addCandidate(paths, pathToUtf8(entry.path()));
        }
    }
#endif
    addWellKnownCandidates(paths);

    std::string normalizedDefault;
    {
        std::vector<std::string> single;
        addCandidate(single, systemDefault);
        if (!single.empty()) normalizedDefault = toLower(single.front());
    }

    std::vector<XRInstalledRuntime> runtimes;
    runtimes.reserve(paths.size());
    for (const std::string& path : paths) {
        XRInstalledRuntime entry;
        entry.manifestPath = path;
        entry.manifestExists = fileExists(path);
        entry.isSystemDefault = !normalizedDefault.empty() && toLower(path) == normalizedDefault;

        std::string name;
        if (entry.manifestExists) readManifest(path, name, entry.libraryPath);
        if (name.empty()) name = vendorNameFromPath(path);
        if (name.empty()) name = folderNameFromPath(path);
        entry.displayName = std::move(name);
        runtimes.push_back(std::move(entry));
    }

    std::sort(runtimes.begin(), runtimes.end(),
              [](const XRInstalledRuntime& a, const XRInstalledRuntime& b) {
                  if (a.isSystemDefault != b.isSystemDefault) return a.isSystemDefault;
                  if (a.manifestExists != b.manifestExists) return a.manifestExists;
                  return toLower(a.displayName) < toLower(b.displayName);
              });
    return runtimes;
}

bool ApplyRuntimeOverride(const std::string& manifestPath, std::string& outError) {
    outError.clear();

    if (manifestPath.empty()) {
        if (!setEnvironmentValue("XR_RUNTIME_JSON", std::string())) {
            outError = "Could not clear XR_RUNTIME_JSON, so the OpenXR runtime override is "
                       "still in effect.";
            return false;
        }
        g_activeOverride.clear();
        return true;
    }

    if (!fileExists(manifestPath)) {
        outError = "OpenXR runtime manifest not found: " + manifestPath +
                   ". Leaving the system default runtime in place.";
        return false;
    }
    if (!setEnvironmentValue("XR_RUNTIME_JSON", manifestPath)) {
        outError = "Could not set XR_RUNTIME_JSON, so the OpenXR runtime could not be "
                   "overridden for this process.";
        return false;
    }
    g_activeOverride = manifestPath;
    return true;
}

const std::string& ActiveRuntimeOverride() { return g_activeOverride; }

void NoteRuntimeLoaded() { g_runtimeLoaded = true; }
bool HasLoadedRuntimeThisSession() { return g_runtimeLoaded; }

}
