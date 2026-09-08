#include "PackageManager.h"

#include <unordered_set>
#include <array>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#pragma region Local Path Helpers
namespace {
fs::path normalizePath(const fs::path& p) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(p, ec);
    if (ec) {
        canonical = fs::absolute(p, ec);
    }
    return canonical.lexically_normal();
}

std::string trimCopy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) start++;
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
    return value.substr(start, end - start);
}

std::vector<std::string> splitTokens(const std::string& input, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(input);
    std::string part;
    while (std::getline(ss, part, delim)) {
        out.push_back(part);
    }
    return out;
}

fs::path resolveManifestPath(const fs::path& projectRoot, const std::string& token) {
    if (token.empty()) return {};
    const fs::path parsed(token);
    if (parsed.is_absolute()) {
        return normalizePath(parsed);
    }
    return normalizePath(projectRoot / parsed);
}

std::string toManifestPathToken(const fs::path& pathValue, const fs::path& projectRoot) {
    if (pathValue.empty()) return "";
    std::error_code ec;
    fs::path rel = fs::relative(pathValue, projectRoot, ec);
    return (!ec ? rel : pathValue).generic_string();
}

bool containsPath(const std::vector<fs::path>& haystack, const fs::path& needle) {
    std::string target = normalizePath(needle).string();
    for (const auto& entry : haystack) {
        if (normalizePath(entry).string() == target) {
            return true;
        }
    }
    return false;
}

bool isGitRepo(const fs::path& root) {
    std::error_code ec;
    fs::path dotGit = root / ".git";
    if (fs::exists(dotGit, ec)) {
        return true;
    }
    return false;
}

std::optional<fs::path> findEngineSourceRoot(const fs::path& start) {
    if (start.empty()) return std::nullopt;

    std::error_code ec;
    fs::path candidate = start;
    while (!candidate.empty()) {
        if (fs::exists(candidate / "CMakeLists.txt", ec) &&
            fs::exists(candidate / "src" / "ScriptRuntime.h", ec)) {
            return candidate;
        }
        fs::path parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return std::nullopt;
}

std::optional<fs::path> findBundledScriptSdkRoot(const fs::path& start) {
    if (start.empty()) return std::nullopt;

    std::error_code ec;
    fs::path candidate = start;
    for (int depth = 0; depth < 6 && !candidate.empty(); ++depth) {
        const fs::path sdkRoot = candidate / "Resources" / "ScriptSDK";
        if (fs::exists(sdkRoot / "src" / "ScriptRuntime.h", ec)) {
            return sdkRoot;
        }
        fs::path parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return std::nullopt;
}

void appendIfExists(std::vector<fs::path>& includeDirs, const fs::path& path) {
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec) || ec) {
        return;
    }
    includeDirs.push_back(normalizePath(path));
}

void appendBundledScriptSdkIncludeDirs(std::vector<fs::path>& includeDirs, const fs::path& sdkRoot) {
    appendIfExists(includeDirs, sdkRoot / "src");
    appendIfExists(includeDirs, sdkRoot / "include");
    appendIfExists(includeDirs, sdkRoot / "src/ThirdParty");
    appendIfExists(includeDirs, sdkRoot / "src/ThirdParty/glm");
    appendIfExists(includeDirs, sdkRoot / "src/ThirdParty/glad");
    appendIfExists(includeDirs, sdkRoot / "src/ThirdParty/glfw/include");
    appendIfExists(includeDirs, sdkRoot / "src/ThirdParty/ModuGUI");
    appendIfExists(includeDirs, sdkRoot / "src/ThirdParty/ModuGUI/backends");
    appendIfExists(includeDirs, sdkRoot / "src/ThirdParty/ImGuizmo");
    appendIfExists(includeDirs, sdkRoot / "src/ThirdParty/assimp/include");
}

fs::path guessIncludeDir(const fs::path& repoRoot, const std::string& includeRel) {
    if (!includeRel.empty()) {
        fs::path candidate = normalizePath(repoRoot / includeRel);
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            return candidate;
        }
    }

    const char* defaults[] = {"include", "Include", "includes", "inc", "Inc"};
    for (const char* name : defaults) {
        fs::path candidate = normalizePath(repoRoot / name);
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            return candidate;
        }
    }

    for (const auto& entry : fs::directory_iterator(repoRoot)) {
        if (entry.is_directory()) {
            return normalizePath(entry.path());
        }
    }

    return normalizePath(repoRoot);
}

bool copyDirectoryRecursive(const fs::path& sourceRoot,
                            const fs::path& destinationRoot,
                            std::string& outError) {
    if (!fs::exists(sourceRoot)) {
        outError = "Source folder does not exist: " + sourceRoot.string();
        return false;
    }

    std::error_code ec;
    fs::create_directories(destinationRoot, ec);
    if (ec) {
        outError = "Failed to create destination folder: " + destinationRoot.string();
        return false;
    }

    for (const auto& entry : fs::recursive_directory_iterator(sourceRoot, ec)) {
        if (ec) {
            outError = "Failed to read source folder: " + sourceRoot.string();
            return false;
        }

        const fs::path rel = fs::relative(entry.path(), sourceRoot, ec);
        if (ec) {
            outError = "Failed to compute relative path for " + entry.path().string();
            return false;
        }

        const fs::path dst = destinationRoot / rel;
        if (entry.is_directory()) {
            fs::create_directories(dst, ec);
            if (ec) {
                outError = "Failed to create directory: " + dst.string();
                return false;
            }
        } else if (entry.is_regular_file()) {
            fs::create_directories(dst.parent_path(), ec);
            if (ec) {
                outError = "Failed to create directory: " + dst.parent_path().string();
                return false;
            }
            fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                outError = "Failed to copy file: " + entry.path().string();
                return false;
            }
        }
    }

    return true;
}

bool parseExternalManifestPackage(const fs::path& projectRoot,
                                  const std::string& payload,
                                  bool modupak,
                                  PackageInfo& outPackage) {
    const auto parts = splitTokens(payload, '|');
    if (parts.size() < 4) return false;

    outPackage = PackageInfo{};
    outPackage.id = trimCopy(parts[0]);
    outPackage.name = trimCopy(parts[1]);
    outPackage.external = true;
    outPackage.modupak = modupak;
    outPackage.localPath = resolveManifestPath(projectRoot, trimCopy(parts[3]));
    outPackage.description = modupak ? "External package from .modupak" : "External package from git";

    const std::string sourceToken = trimCopy(parts[2]);
    if (modupak) {
        outPackage.modupakSourcePath = resolveManifestPath(projectRoot, sourceToken);
    } else {
        outPackage.gitUrl = sourceToken;
    }

    if (parts.size() > 4) {
        for (const auto& inc : splitTokens(parts[4], ';')) {
            const std::string cleaned = trimCopy(inc);
            if (cleaned.empty()) continue;
            outPackage.includeDirs.push_back(resolveManifestPath(projectRoot, cleaned));
        }
    }
    auto readCleanList = [](const std::string& raw) {
        std::vector<std::string> vals;
        for (const std::string& token : splitTokens(raw, ';')) {
            const std::string cleaned = trimCopy(token);
            if (!cleaned.empty()) vals.push_back(cleaned);
        }
        return vals;
    };

    if (parts.size() > 5) {
        outPackage.defines = readCleanList(parts[5]);
    }
    if (parts.size() > 6) {
        outPackage.linuxLibs = readCleanList(parts[6]);
    }
    if (parts.size() > 7) {
        outPackage.windowsLibs = readCleanList(parts[7]);
    }
    if (parts.size() > 8) {
        const std::string desc = trimCopy(parts[8]);
        if (!desc.empty()) outPackage.description = desc;
    }

    if (outPackage.id.empty()) return false;
    if (outPackage.name.empty()) outPackage.name = outPackage.id;
    if (outPackage.includeDirs.empty()) {
        outPackage.includeDirs.push_back(guessIncludeDir(outPackage.localPath, "include"));
    }
    return true;
}

std::string stripLineComment(const std::string& line) {
    bool inString = false;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
            inString = !inString;
        }
        if (!inString && line[i] == '/' && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string unquoteValue(std::string value) {
    value = trimCopy(value);
    if (!value.empty() && value.back() == ';') {
        value.pop_back();
        value = trimCopy(value);
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool parseAssignment(const std::string& line, std::string& outKey, std::string& outValue) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
        return false;
    }
    outKey = trimCopy(line.substr(0, eq));
    outValue = unquoteValue(line.substr(eq + 1));
    return !outKey.empty();
}

bool lineContainsBlockHeader(const std::string& line, const std::string& header) {
    return trimCopy(line).find(header) != std::string::npos;
}

int countChar(const std::string& line, char target) {
    return static_cast<int>(std::count(line.begin(), line.end(), target));
}

std::string normalizeVersionToken(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            out.push_back(c);
        }
    }
    return out;
}

bool startsWithInsensitive(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool isHttpUrl(const std::string& value) {
    return startsWithInsensitive(value, "http://") || startsWithInsensitive(value, "https://");
}

std::string joinUrl(std::string base, const std::string& tail) {
    if (tail.empty()) {
        return base;
    }
    if (isHttpUrl(tail)) {
        return tail;
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (!tail.empty() && tail.front() == '/') {
        return base + tail;
    }
    return base + "/" + tail;
}
} // namespace
#pragma endregion

bool hasVersionedPackageAtAnyVersion(const fs::path& root, const PackageInfo& pkg);
std::optional<fs::path> findVersionedPackageAtAnyVersion(const fs::path& root, const PackageInfo& pkg);

#pragma region Lifecycle
PackageManager::PackageManager() {
#if !MODULARITY_RUNTIME_ONLY
    buildRegistry();
#endif
}

void PackageManager::setProjectRoot(const fs::path& root) {
    const auto __t0 = std::chrono::steady_clock::now();
#if !MODULARITY_RUNTIME_ONLY
    buildRegistry();
#endif
    const auto __t1 = std::chrono::steady_clock::now();
    projectRoot = root;
    manifestPath = projectRoot / "packages.modu";
#if !MODULARITY_RUNTIME_ONLY
    loadManifest();
#endif
    const auto __t2 = std::chrono::steady_clock::now();
    const double buildMs = std::chrono::duration<double, std::milli>(__t1 - __t0).count();
    const double manifestMs = std::chrono::duration<double, std::milli>(__t2 - __t1).count();
    std::fprintf(stderr,
                 "[PackageManager] setProjectRoot: buildRegistry=%.1f ms loadManifest=%.1f ms\n",
                 buildMs, manifestMs);
    std::fflush(stderr);
}

void PackageManager::refreshRegistry() {
#if MODULARITY_RUNTIME_ONLY
    return;
#else
    allowRemoteRegistryFetch = true;
    buildRegistry();
    allowRemoteRegistryFetch = false;
    if (!projectRoot.empty()) {
        manifestPath = projectRoot / "packages.modu";
        loadManifest();
    }
#endif
}
#pragma endregion

#pragma region Install / Remove
bool PackageManager::isInstalled(const std::string& id) const {
    return std::find(installedIds.begin(), installedIds.end(), id) != installedIds.end();
}

bool PackageManager::hasProjectInstallPayload(const std::string& id) const {
    const PackageInfo* pkg = findPackage(id);
    return pkg && pkg->registryPackage && hasVersionedPackageAtAnyVersion(packagesFolder(), *pkg);
}

bool PackageManager::isGloballyInstalled(const std::string& id) const {
    const PackageInfo* pkg = findPackage(id);
    if (!pkg || !pkg->registryPackage) {
        return false;
    }
    return findVersionedPackageAtAnyVersion(globalPackagesFolder(), *pkg).has_value();
}

bool PackageManager::hasUpdateAvailable(const std::string& id) const {
    const PackageInfo* pkg = findPackage(id);
    if (!pkg || !pkg->registryPackage) {
        return false;
    }
    const bool hasProjectLegacy = hasLegacyVersionedPackageAt(packagesFolder(), *pkg);
    const bool hasGlobalLegacy = hasLegacyVersionedPackageAt(globalPackagesFolder() / currentEngineVersion() / pkg->author, *pkg) ||
                                 hasLegacyVersionedPackageAt(globalPackagesFolder(), *pkg);
    return hasProjectLegacy || hasGlobalLegacy;
}

bool PackageManager::isCompatible(const std::string& id) const {
    const PackageInfo* pkg = findPackage(id);
    return pkg && isCompatible(*pkg);
}

bool PackageManager::isCompatible(const PackageInfo& pkg) const {
    if (!pkg.registryPackage) {
        return true;
    }
    return isCompatibleVersionString(pkg.compatibleModuEngineVersion, currentEngineVersion());
}

bool PackageManager::install(const std::string& id) {
    lastError.clear();
    const PackageInfo* pkg = findPackage(id);
    if (!pkg) {
        lastError = "Unknown package: " + id;
        return false;
    }
    if (isInstalled(id)) {
        return true;
    }
    installedIds.push_back(id);
    saveManifest();
    return true;
}

bool PackageManager::installRegistryPackageToProject(const std::string& id) {
    lastError.clear();
    if (!ensureProjectRoot()) {
        lastError = "Project root not set.";
        return false;
    }

    const PackageInfo* pkg = findPackage(id);
    if (!pkg || !pkg->registryPackage) {
        lastError = "Package is not available in the ModuEngine registry: " + id;
        return false;
    }
    if (!isCompatible(*pkg)) {
        lastError = "Package is incompatible with ModuEngine " + currentEngineVersion() + ": " + id;
        return false;
    }

    const fs::path destination = projectRegistryPackagePath(*pkg);
    if (!fs::exists(destination)) {
        fs::path source = isGloballyInstalled(id) ? findVersionedPackageAtAnyVersion(globalPackagesFolder(), *pkg).value_or(fs::path())
                                                  : resolveRegistrySourcePath(*pkg);
        if ((source.empty() || !fs::exists(source)) && !pkg->downloadUrl.empty()) {
            if (!downloadRegistryPackageToCache(*pkg, source)) {
                return false;
            }
        }
        if (source.empty() || !fs::exists(source)) {
            lastError = "Package files were not found for " + id + ".";
            return false;
        }

        std::string copyError;
        if (!copyDirectoryRecursive(source, destination, copyError)) {
            lastError = copyError;
            return false;
        }
    }

    return install(id);
}

bool PackageManager::installRegistryPackageGlobally(const std::string& id) {
    lastError.clear();

    const PackageInfo* pkg = findPackage(id);
    if (!pkg || !pkg->registryPackage) {
        lastError = "Package is not available in the ModuEngine registry: " + id;
        return false;
    }
    if (!isCompatible(*pkg)) {
        lastError = "Package is incompatible with ModuEngine " + currentEngineVersion() + ": " + id;
        return false;
    }

    const fs::path destination = globalRegistryPackagePath(*pkg);
    if (fs::exists(destination)) {
        return true;
    }

    fs::path source = resolveRegistrySourcePath(*pkg);
    if ((source.empty() || !fs::exists(source)) && !pkg->downloadUrl.empty()) {
        if (!downloadRegistryPackageToCache(*pkg, source)) {
            return false;
        }
    }
    if (source.empty() || !fs::exists(source)) {
        lastError = "Package files were not found for " + id + ".";
        return false;
    }

    std::string copyError;
    if (!copyDirectoryRecursive(source, destination, copyError)) {
        lastError = copyError;
        return false;
    }
    return true;
}

bool PackageManager::remove(const std::string& id) {
    lastError.clear();
    const PackageInfo* pkg = findPackage(id);
    if (!pkg) {
        lastError = "Unknown package: " + id;
        return false;
    }
    if (pkg->builtIn) {
        lastError = "Cannot remove built-in packages.";
        return false;
    }
    if (pkg->registryPackage) {
        const fs::path root = packagesFolder();
        std::error_code ec;
        fs::remove_all(projectRegistryPackagePath(*pkg), ec);
        if (fs::exists(root, ec) && fs::is_directory(root, ec)) {
            const std::string prefix = pkg->id + "-";
            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (ec || !entry.is_directory()) {
                    continue;
                }
                const std::string folder = entry.path().filename().string();
                if (folder.rfind(prefix, 0) == 0) {
                    std::error_code removeEc;
                    fs::remove_all(entry.path(), removeEc);
                }
            }
        }
    } else if (pkg->external) {
        std::string log;
        const bool isGitExternal = !pkg->gitUrl.empty();
        if (isGitExternal && isGitRepo(projectRoot)) {
            std::error_code ec;
            fs::path relPath = fs::relative(pkg->localPath, projectRoot, ec);
            std::string rel = (!ec ? relPath : pkg->localPath).generic_string();
            std::string deinitCmd = "git -C \"" + projectRoot.string() + "\" submodule deinit -f \"" + rel + "\"";
            runCommand(deinitCmd, log); // best-effort
            std::string rmCmd = "git -C \"" + projectRoot.string() + "\" rm -f \"" + rel + "\"";
            if (!runCommand(rmCmd, log)) {
                lastError = "Failed to remove submodule. Git log:\n" + log;
                return false;
            }
        } else {
            std::error_code ec;
            fs::remove_all(pkg->localPath, ec);
            if (ec) {
                lastError = "Failed to remove package folder: " + ec.message();
                return false;
            }
        }
    }

    auto it = std::remove(installedIds.begin(), installedIds.end(), id);
    if (it == installedIds.end()) {
        return true;
    }
    installedIds.erase(it, installedIds.end());
    registry.erase(std::remove_if(registry.begin(), registry.end(),
        [&](const PackageInfo& p){ return p.id == id && p.external; }), registry.end());

    saveManifest();
    return true;
}

bool PackageManager::removeRegistryPackageGlobally(const std::string& id) {
    lastError.clear();
    const PackageInfo* pkg = findPackage(id);
    if (!pkg || !pkg->registryPackage) {
        lastError = "Package is not available in the ModuEngine registry: " + id;
        return false;
    }

    std::error_code ec;
    const fs::path globalRoot = globalPackagesFolder();
    if (!fs::exists(globalRoot, ec) || !fs::is_directory(globalRoot, ec)) {
        return true;
    }

    const fs::path expected = registryPackageFolderName(*pkg);
    if (!expected.empty()) {
        for (const auto& versionDir : fs::directory_iterator(globalRoot, ec)) {
            if (ec || !versionDir.is_directory()) {
                ec.clear();
                continue;
            }

            const fs::path direct = versionDir.path() / expected;
            if (fs::exists(direct, ec) && fs::is_directory(direct, ec)) {
                fs::remove_all(direct, ec);
            }

            if (pkg->author.empty()) {
                continue;
            }

            const fs::path authorRoot = versionDir.path() / pkg->author;
            if (!fs::exists(authorRoot, ec) || !fs::is_directory(authorRoot, ec)) {
                ec.clear();
                continue;
            }

            for (const auto& entry : fs::directory_iterator(authorRoot, ec)) {
                if (ec || !entry.is_directory()) {
                    ec.clear();
                    continue;
                }
                const std::string folder = entry.path().filename().string();
                if (folder == expected.filename().string() || folder.rfind(pkg->id + "-", 0) == 0) {
                    std::error_code removeEc;
                    fs::remove_all(entry.path(), removeEc);
                }
            }
        }
    }
    return true;
}
#pragma endregion

#pragma region Build Config
void PackageManager::applyToBuildConfig(ScriptBuildConfig& config) const {
    std::unordered_set<std::string> defineSet(config.defines.begin(), config.defines.end());
    std::unordered_set<std::string> linuxLibSet(config.linuxLinkLibs.begin(), config.linuxLinkLibs.end());
    std::unordered_set<std::string> winLibSet(config.windowsLinkLibs.begin(), config.windowsLinkLibs.end());

    for (const auto& id : installedIds) {
        const PackageInfo* pkg = findPackage(id);
        if (!pkg) continue;

        std::vector<fs::path> resolvedIncludeDirs = pkg->includeDirs;
        if (pkg->registryPackage && resolvedIncludeDirs.empty()) {
            auto appendIncludeRoot = [&](const fs::path& candidate) {
                if (candidate.empty()) return;
                std::error_code ec;
                if (!fs::exists(candidate, ec) || !fs::is_directory(candidate, ec)) {
                    return;
                }
                const fs::path normalized = normalizePath(candidate);
                if (!containsPath(resolvedIncludeDirs, normalized)) {
                    resolvedIncludeDirs.push_back(normalized);
                }
            };

            fs::path packageRoot = projectRegistryPackagePath(*pkg);
            std::error_code ec;
            if (!fs::exists(packageRoot, ec) || !fs::is_directory(packageRoot, ec)) {
                packageRoot = findVersionedPackageAtAnyVersion(globalPackagesFolder(), *pkg).value_or(fs::path());
            }
            if (packageRoot.empty()) {
                packageRoot = pkg->registrySourcePath;
            }

            appendIncludeRoot(packageRoot);
            appendIncludeRoot(packageRoot / "include");
            appendIncludeRoot(packageRoot / "inc");
        }

        for (const auto& dir : resolvedIncludeDirs) {
            if (!containsPath(config.includeDirs, dir)) {
                config.includeDirs.push_back(normalizePath(dir));
            }
        }
        for (const auto& def : pkg->defines) {
            if (defineSet.insert(def).second) {
                config.defines.push_back(def);
            }
        }
        for (const auto& lib : pkg->linuxLibs) {
            if (linuxLibSet.insert(lib).second) {
                config.linuxLinkLibs.push_back(lib);
            }
        }
        for (const auto& lib : pkg->windowsLibs) {
            if (winLibSet.insert(lib).second) {
                config.windowsLinkLibs.push_back(lib);
            }
        }
    }
}
#pragma endregion

#pragma region Registry
void PackageManager::buildRegistry() {
    registry.clear();
    registryAvailable = false;
    registryStatus.clear();
    registryLastUpdated.clear();
    registryUpdatedBy.clear();
    registryRoot.clear();
    const fs::path runtimeRoot = fs::current_path();
    const fs::path engineSourceRoot = findEngineSourceRoot(runtimeRoot).value_or(fs::path());
    const fs::path sdkRoot = findBundledScriptSdkRoot(runtimeRoot).value_or(fs::path());

    auto add = [this](PackageInfo info) {
        for (auto& dir : info.includeDirs) {
            dir = normalizePath(dir);
        }
        registry.push_back(std::move(info));
    };

    PackageInfo engineCore;
    engineCore.id = "engine-core";
    engineCore.name = "Engine Core";
    engineCore.description = "Modularity engine headers and common utilities";
    engineCore.builtIn = true;
    if (!engineSourceRoot.empty()) {
        appendBundledScriptSdkIncludeDirs(engineCore.includeDirs, engineSourceRoot);
    }
    if (!sdkRoot.empty() && sdkRoot != engineSourceRoot) {
        appendBundledScriptSdkIncludeDirs(engineCore.includeDirs, sdkRoot);
    }
    engineCore.linuxLibs = {"pthread", "dl"};
    engineCore.windowsLibs = {"User32.lib", "Advapi32.lib"};
    add(engineCore);

    PackageInfo glm;
    glm.id = "glm";
    glm.name = "GLM Math";
    glm.description = "Header-only GLM math library (bundled)";
    glm.builtIn = false; // Count as installed instead of hidden built-in
    if (!engineSourceRoot.empty()) {
        appendIfExists(glm.includeDirs, engineSourceRoot / "src/ThirdParty/glm");
    } else if (!sdkRoot.empty()) {
        appendIfExists(glm.includeDirs, sdkRoot / "src/ThirdParty/glm");
    }
    add(glm);

    PackageInfo imgui;
    imgui.id = "imgui";
    imgui.name = "ModuGUI";
    imgui.description = "ModuGUI immediate-mode UI fork for editor-time tools";
    imgui.builtIn = false;
    if (!engineSourceRoot.empty()) {
        appendIfExists(imgui.includeDirs, engineSourceRoot / "src/ThirdParty/ModuGUI");
        appendIfExists(imgui.includeDirs, engineSourceRoot / "src/ThirdParty/ModuGUI/backends");
    } else if (!sdkRoot.empty()) {
        appendIfExists(imgui.includeDirs, sdkRoot / "src/ThirdParty/ModuGUI");
        appendIfExists(imgui.includeDirs, sdkRoot / "src/ThirdParty/ModuGUI/backends");
    }
    add(imgui);

    PackageInfo imguizmo;
    imguizmo.id = "imguizmo";
    imguizmo.name = "ImGuizmo";
    imguizmo.description = "Gizmo/transform helpers used by the editor";
    imguizmo.builtIn = false;
    if (!engineSourceRoot.empty()) {
        appendIfExists(imguizmo.includeDirs, engineSourceRoot / "src/ThirdParty/ImGuizmo");
    } else if (!sdkRoot.empty()) {
        appendIfExists(imguizmo.includeDirs, sdkRoot / "src/ThirdParty/ImGuizmo");
    }
    add(imguizmo);

    PackageInfo miniaudio;
    miniaudio.id = "miniaudio";
    miniaudio.name = "miniaudio";
    miniaudio.description = "Single-header audio helpers (bundled)";
    miniaudio.builtIn = false;
    if (!engineSourceRoot.empty()) {
        appendIfExists(miniaudio.includeDirs, engineSourceRoot / "include/ThirdParty");
    } else if (!sdkRoot.empty()) {
        appendIfExists(miniaudio.includeDirs, sdkRoot / "include/ThirdParty");
    }
    add(miniaudio);

    auto addOptionalEnginePackage = [&](const char* id,
                                        const char* name,
                                        const char* description) {
        PackageInfo pkg;
        pkg.id = id;
        pkg.name = name;
        pkg.description = description;
        pkg.builtIn = false;
        add(std::move(pkg));
    };

    addOptionalEnginePackage("moduengine.sprite-editor",
                             "Pixel Sprite Editor",
                             "Optional sprite editing window and tools.");
    addOptionalEnginePackage("moduengine.spritesheet",
                             "Spritesheet Tools",
                             "Optional spritesheet import and sidecar tooling.");
    addOptionalEnginePackage("moduengine.2d-world",
                             "2D World Essentials",
                             "Optional 2D world rendering and Light2D tooling.");
    addOptionalEnginePackage("moduengine.mesh-builder",
                             "Mesh Builder",
                             "Optional mesh builder and mesh editing tools.");
    addOptionalEnginePackage("moduengine.scripting-window",
                             "Scripting Window",
                             "Optional in-editor scripting window.");
    addOptionalEnginePackage("moduengine.vulkan-pipeline",
                             "Vulkan Pipeline",
                             "Optional experimental Vulkan rendering pipeline.");

    loadRegistryMetadata(!engineSourceRoot.empty() ? engineSourceRoot : runtimeRoot);
}

bool PackageManager::loadRegistryMetadata(const fs::path& engineRoot) {
    const std::string engineVersion = currentEngineVersion();
    registryRoot = findRegistryRoot(engineRoot);

    fs::path listingPath;
    bool usingRemoteListing = false;
    std::string remoteLog;
    std::error_code cacheEc;
    const fs::path cachedListing = packageCacheFolder() / "Metadata" / "PackageManagerInfo.modu";
    if (!allowRemoteRegistryFetch && fs::exists(cachedListing, cacheEc) && !cacheEc) {
        listingPath = cachedListing;
        usingRemoteListing = true;
    } else if (allowRemoteRegistryFetch && downloadRemoteRegistryListing(listingPath, remoteLog)) {
        usingRemoteListing = true;
    } else if (!registryRoot.empty() && fs::exists(registryRoot / "PackageManagerInfo.modu")) {
        listingPath = registryRoot / "PackageManagerInfo.modu";
    } else if (downloadRemoteRegistryListing(listingPath, remoteLog)) {
        usingRemoteListing = true;
    } else {
        registryStatus = "Registry unavailable. Remote fetch failed";
        if (!remoteLog.empty()) {
            registryStatus += " (" + remoteLog + ")";
        }
        registryStatus += ", and no local registry checkout was found.";
        return false;
    }

    std::ifstream file(listingPath);
    if (!file.is_open()) {
        registryStatus = "Failed to open registry metadata: " + listingPath.string();
        return false;
    }

    int braceDepth = 0;
    int versionDepth = -1;
    int packageDepth = -1;
    bool pendingVersionBlock = false;
    bool inVersionBlock = false;
    bool pendingPackageBlock = false;
    bool inPackageBlock = false;
    PackageInfo currentPackage;

    auto extractVersionBlock = [](const std::string& line) -> std::string {
        const std::string marker = "ModuEngineVersion(";
        const size_t start = line.find(marker);
        if (start == std::string::npos) return {};
        const size_t firstQuote = line.find('"', start + marker.size());
        if (firstQuote == std::string::npos) return {};
        const size_t secondQuote = line.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos || secondQuote <= firstQuote + 1) return {};
        return trim(line.substr(firstQuote + 1, secondQuote - firstQuote - 1));
    };

    std::string currentBlockVersion;
    std::unordered_map<std::string, std::string> entryBlockVersion;

    auto finalizePackage = [&]() {
        if (currentPackage.id.empty()) {
            currentPackage = PackageInfo{};
            return;
        }

        currentPackage.registryPackage = true;
        currentPackage.registryEngineVersion = engineVersion;
        currentPackage.registrySourcePath = resolveRegistrySourcePath(currentPackage);

        auto existing = std::find_if(registry.begin(), registry.end(), [&](const PackageInfo& entry) {
            return entry.id == currentPackage.id;
        });

        if (existing != registry.end()) {
            const auto prior = entryBlockVersion.find(currentPackage.id);
            const bool newBlockIsNewer =
                prior == entryBlockVersion.end() ||
                prior->second.empty() ||
                currentBlockVersion.empty() ||
                isCompatibleVersionString(prior->second, currentBlockVersion);
            if (!newBlockIsNewer) {
                currentPackage = PackageInfo{};
                return;
            }
            entryBlockVersion[currentPackage.id] = currentBlockVersion;
            if (!currentPackage.name.empty()) existing->name = currentPackage.name;
            if (!currentPackage.description.empty()) existing->description = currentPackage.description;
            existing->author = currentPackage.author;
            existing->packageType = currentPackage.packageType;
            existing->subsystem = currentPackage.subsystem;
            existing->version = currentPackage.version;
            existing->compatibleModuEngineVersion = currentPackage.compatibleModuEngineVersion;
            existing->registryEngineVersion = currentPackage.registryEngineVersion;
            existing->downloadUrl = currentPackage.downloadUrl;
            existing->archiveType = currentPackage.archiveType;
            existing->checksum = currentPackage.checksum;
            existing->registryPackage = true;
            existing->registrySourcePath = currentPackage.registrySourcePath;
        } else {
            entryBlockVersion[currentPackage.id] = currentBlockVersion;
            registry.push_back(currentPackage);
        }

        currentPackage = PackageInfo{};
    };

    std::string rawLine;
    while (std::getline(file, rawLine)) {
        const std::string line = trim(stripLineComment(rawLine));
        if (line.empty()) {
            continue;
        }

        std::string key;
        std::string value;
        if (!inVersionBlock && parseAssignment(line, key, value)) {
            if (key == "PackageInfoLastUpdated") {
                registryLastUpdated = value;
            } else if (key == "UpdatedBy") {
                registryUpdatedBy = value;
            }
        }

        if (!inVersionBlock && lineContainsBlockHeader(line, "ModuEngineVersion(")) {
            const std::string versionToken = extractVersionBlock(line);
            pendingVersionBlock = isCompatibleVersionString(versionToken, engineVersion);
            if (pendingVersionBlock) {
                currentBlockVersion = versionToken;
            }
        } else if (inVersionBlock && !inPackageBlock && lineContainsBlockHeader(line, "Package()")) {
            pendingPackageBlock = true;
            currentPackage = PackageInfo{};
        }

        const int opens = countChar(line, '{');
        const int closes = countChar(line, '}');

        if (pendingVersionBlock && opens > 0) {
            inVersionBlock = true;
            pendingVersionBlock = false;
            versionDepth = braceDepth + 1;
        }
        if (inVersionBlock && pendingPackageBlock && opens > 0) {
            inPackageBlock = true;
            pendingPackageBlock = false;
            packageDepth = braceDepth + 1;
        }

        if (inPackageBlock && parseAssignment(line, key, value)) {
            if (key == "PackageID") currentPackage.id = value;
            else if (key == "Name") currentPackage.name = value;
            else if (key == "Author") currentPackage.author = value;
            else if (key == "Description") currentPackage.description = value;
            else if (key == "PackageType") currentPackage.packageType = value;
            else if (key == "Subsystem") currentPackage.subsystem = value;
            else if (key == "Version") currentPackage.version = value;
            else if (key == "CompatibleModuEngineVersion") currentPackage.compatibleModuEngineVersion = value;
            else if (key == "DownloadURL") currentPackage.downloadUrl = joinUrl(configuredRegistryBaseUrl(), value);
            else if (key == "ArchiveType") currentPackage.archiveType = value;
            else if (key == "Checksum") currentPackage.checksum = value;
        }

        braceDepth += opens - closes;

        if (inPackageBlock && braceDepth < packageDepth) {
            finalizePackage();
            inPackageBlock = false;
            packageDepth = -1;
        }
        if (inVersionBlock && braceDepth < versionDepth) {
            inVersionBlock = false;
            versionDepth = -1;
        }
    }

    if (inPackageBlock) {
        finalizePackage();
    }

    size_t registryPackageCount = 0;
    for (const auto& pkg : registry) {
        if (pkg.registryPackage) {
            ++registryPackageCount;
        }
    }

    registryAvailable = registryPackageCount > 0;
    if (registryAvailable) {
        registryStatus = "Loaded " + std::to_string(registryPackageCount) +
                         " registry package" + (registryPackageCount == 1 ? "" : "s") +
                         " for ModuEngine " + engineVersion +
                         (usingRemoteListing ? " from the online registry." : " from the local registry.");
        if (!usingRemoteListing && !remoteLog.empty()) {
            registryStatus += " Remote fetch failed: " + remoteLog;
        }
    } else {
        registryStatus = "Registry metadata loaded, but no packages were found for ModuEngine " + engineVersion + ".";
    }
    return registryAvailable;
}

fs::path PackageManager::findRegistryRoot(const fs::path& start) const {
    std::vector<fs::path> candidates;

    if (const char* envRoot = std::getenv("MODUENGINE_PACKAGE_REGISTRY_ROOT")) {
        if (*envRoot) {
            candidates.emplace_back(envRoot);
        }
    }

    const fs::path normalizedStart = normalizePath(start.empty() ? fs::current_path() : start);
    fs::path current = normalizedStart;
    for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
        candidates.push_back(current);
        candidates.push_back(current / "Modu-Package-Manager");
        candidates.push_back(current.parent_path() / "Modu-Package-Manager");
        const fs::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        candidates.emplace_back(fs::path(appdata) / ".Modularity" / "PackageRegistry");
    }
#else
    if (const char* home = std::getenv("HOME")) {
        candidates.emplace_back(fs::path(home) / ".Modularity" / "PackageRegistry");
    }
#endif

    std::unordered_set<std::string> seen;
    for (const fs::path& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        const fs::path normalized = normalizePath(candidate);
        if (!seen.insert(normalized.string()).second) {
            continue;
        }
        if (fs::exists(normalized / "PackageManagerInfo.modu") &&
            fs::exists(normalized / "Packages")) {
            return normalized;
        }
    }

    return {};
}

fs::path PackageManager::resolveRegistrySourcePath(const PackageInfo& pkg) const {
    if (!pkg.registryPackage || registryRoot.empty()) {
        return {};
    }

    std::error_code ec;
    const fs::path packageFolder = registryPackageFolderName(pkg);
    if (packageFolder.empty()) {
        return {};
    }

    const fs::path packagesRoot = registryRoot / "Packages";
    const fs::path preferredRoot = packagesRoot / currentEngineVersion();
    auto searchRoot = [&](const fs::path& versionRoot) -> fs::path {
        if (!fs::exists(versionRoot, ec) || !fs::is_directory(versionRoot, ec)) {
            ec.clear();
            return {};
        }
        if (!pkg.author.empty()) {
            const fs::path direct = versionRoot / pkg.author / packageFolder;
            if (fs::exists(direct)) {
                return normalizePath(direct);
            }
        }
        for (const auto& authorDir : fs::directory_iterator(versionRoot, ec)) {
            if (ec || !authorDir.is_directory()) {
                ec.clear();
                continue;
            }
            const fs::path candidate = authorDir.path() / packageFolder;
            if (fs::exists(candidate)) {
                return normalizePath(candidate);
            }
        }
        ec.clear();
        return {};
    };

    if (fs::path direct = searchRoot(preferredRoot); !direct.empty()) {
        return direct;
    }

    if (fs::exists(packagesRoot, ec) && fs::is_directory(packagesRoot, ec)) {
        for (const auto& versionDir : fs::directory_iterator(packagesRoot, ec)) {
            if (ec || !versionDir.is_directory()) {
                ec.clear();
                continue;
            }
            const fs::path candidate = searchRoot(versionDir.path());
            if (!candidate.empty()) {
                return candidate;
            }
        }
    }

    return {};
}

bool PackageManager::resolveRegistryPackageLocations(const std::string& id, RegistryPackageLocations& out) const {
    out = {};
    const PackageInfo* pkg = findPackage(id);
    if (!pkg || !pkg->registryPackage) {
        return false;
    }
    out.destination = projectRegistryPackagePath(*pkg);
    out.downloadUrl = pkg->downloadUrl;
    fs::path source = resolveRegistrySourcePath(*pkg);
    if (source.empty() || !fs::exists(source)) {
        auto globalSource = findVersionedPackageAtAnyVersion(globalPackagesFolder(), *pkg);
        if (globalSource.has_value() && fs::exists(*globalSource)) {
            source = *globalSource;
        }
    }
    out.source = source;
    return true;
}

fs::path PackageManager::projectRegistryPackagePath(const PackageInfo& pkg) const {
    if (projectRoot.empty()) {
        return {};
    }
    return normalizePath(packagesFolder() / registryPackageFolderName(pkg));
}

fs::path PackageManager::globalPackagesFolder() const {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) {
        return normalizePath(fs::path(appdata) / ".Modularity" / "Packages");
    }
#else
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return normalizePath(fs::path(home) / ".Modularity" / "Packages");
    }
#endif
    return normalizePath(fs::current_path() / ".Modularity" / "Packages");
}

fs::path PackageManager::packageCacheFolder() const {
    return normalizePath(globalPackagesFolder().parent_path() / "Cache" / "PackageRegistry");
}

fs::path PackageManager::globalRegistryPackagePath(const PackageInfo& pkg) const {
    return normalizePath(globalPackagesFolder() / currentEngineVersion() / pkg.author / registryPackageFolderName(pkg));
}

fs::path PackageManager::registryPackageFolderName(const PackageInfo& pkg) const {
    if (pkg.id.empty() || pkg.version.empty()) {
        return {};
    }
    return fs::path(pkg.id + "-" + pkg.version);
}

bool PackageManager::hasVersionedPackageAt(const fs::path& root, const PackageInfo& pkg) const {
    const fs::path folder = root / registryPackageFolderName(pkg);
    std::error_code ec;
    return fs::exists(folder, ec) && fs::is_directory(folder, ec);
}

bool PackageManager::hasLegacyVersionedPackageAt(const fs::path& root, const PackageInfo& pkg) const {
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return false;
    }
    const std::string expected = registryPackageFolderName(pkg).string();
    const std::string prefix = pkg.id + "-";
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name == expected) {
            continue;
        }
        if (name.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool hasVersionedPackageAtAnyVersion(const fs::path& root, const PackageInfo& pkg) {
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return false;
    }

    const fs::path expected = pkg.id.empty() || pkg.version.empty()
        ? fs::path()
        : fs::path(pkg.id + "-" + pkg.version);
    if (!expected.empty() && fs::exists(root / expected, ec) && fs::is_directory(root / expected, ec)) {
        return true;
    }

    for (const auto& versionDir : fs::directory_iterator(root, ec)) {
        if (ec || !versionDir.is_directory()) {
            ec.clear();
            continue;
        }
        const fs::path candidate = versionDir.path() / expected;
        if (!expected.empty() && fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
            return true;
        }
        if (pkg.author.empty()) {
            continue;
        }
        const fs::path authorCandidate = versionDir.path() / pkg.author / expected;
        if (!expected.empty() && fs::exists(authorCandidate, ec) && fs::is_directory(authorCandidate, ec)) {
            return true;
        }
        ec.clear();
    }
    return false;
}

std::optional<fs::path> findVersionedPackageAtAnyVersion(const fs::path& root, const PackageInfo& pkg) {
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return std::nullopt;
    }

    const fs::path expected = pkg.id.empty() || pkg.version.empty()
        ? fs::path()
        : fs::path(pkg.id + "-" + pkg.version);
    if (!expected.empty() && fs::exists(root / expected, ec) && fs::is_directory(root / expected, ec)) {
        return normalizePath(root / expected);
    }

    for (const auto& versionDir : fs::directory_iterator(root, ec)) {
        if (ec || !versionDir.is_directory()) {
            ec.clear();
            continue;
        }

        const fs::path direct = versionDir.path() / expected;
        if (!expected.empty() && fs::exists(direct, ec) && fs::is_directory(direct, ec)) {
            return normalizePath(direct);
        }

        if (pkg.author.empty()) {
            continue;
        }

        const fs::path authorRoot = versionDir.path() / pkg.author;
        if (!fs::exists(authorRoot, ec) || !fs::is_directory(authorRoot, ec)) {
            ec.clear();
            continue;
        }

        const fs::path authorDirect = authorRoot / expected;
        if (!expected.empty() && fs::exists(authorDirect, ec) && fs::is_directory(authorDirect, ec)) {
            return normalizePath(authorDirect);
        }

        for (const auto& entry : fs::directory_iterator(authorRoot, ec)) {
            if (ec || !entry.is_directory()) {
                ec.clear();
                continue;
            }
            const std::string folder = entry.path().filename().string();
            if (folder == expected.filename().string() || folder.rfind(pkg.id + "-", 0) == 0) {
                return normalizePath(entry.path());
            }
        }

        ec.clear();
    }

    return std::nullopt;
}

fs::path PackageManager::registryPackageCachePath(const PackageInfo& pkg) const {
    return normalizePath(packageCacheFolder() / "Extracted" / registryPackageFolderName(pkg));
}

std::string PackageManager::configuredRegistryBaseUrl() const {
    if (const char* envUrl = std::getenv("MODUENGINE_PACKAGE_REGISTRY_URL")) {
        if (*envUrl) {
            return trim(envUrl);
        }
    }
    return "https://pak.moduengine.xyz/Tareno-Labs-LLC/Modu-Package-Manager";
}

std::string PackageManager::configuredRegistryMetadataUrl() const {
    if (const char* envUrl = std::getenv("MODUENGINE_PACKAGE_REGISTRY_METADATA_URL")) {
        if (*envUrl) {
            return trim(envUrl);
        }
    }
    return "https://pak.moduengine.xyz/Tareno-Labs-LLC/Modu-Package-Manager/raw/branch/main/PackageManagerInfo.modu";
}

bool PackageManager::downloadRemoteRegistryListing(fs::path& outListingPath, std::string& outLog) const {
    outListingPath.clear();
    outLog.clear();

    const fs::path cacheDir = packageCacheFolder() / "Metadata";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    if (ec) {
        outLog = "failed to create registry cache folder";
        return false;
    }

    const fs::path listingPath = cacheDir / "PackageManagerInfo.modu";
    const std::string url = configuredRegistryMetadataUrl();
    if (!downloadFile(url, listingPath, outLog)) {
        return false;
    }

    outListingPath = listingPath;
    return true;
}

bool PackageManager::downloadFile(const std::string& url, const fs::path& destination, std::string& outLog) const {
    outLog.clear();
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        outLog = "failed to create download folder";
        return false;
    }

    std::string escapedUrl = url;
    std::string escapedDestination = destination.string();
    const std::string command =
        "curl -fsSL --retry 2 --connect-timeout 10 \"" + escapedUrl + "\" -o \"" + escapedDestination + "\" 2>&1";
    if (!runCommand(command, outLog)) {
        if (outLog.empty()) {
            outLog = "curl download failed";
        }
        return false;
    }
    return fs::exists(destination);
}

bool PackageManager::downloadRegistryPackageToCache(const PackageInfo& pkg, fs::path& outSourcePath) {
    lastError.clear();
    outSourcePath.clear();

    if (pkg.downloadUrl.empty()) {
        lastError = "Package is missing a DownloadURL: " + pkg.id;
        return false;
    }

    fs::path extractedRoot = registryPackageCachePath(pkg);
    fs::path resolvedCachedRoot = resolveExtractedPackageRoot(extractedRoot);
    if (!resolvedCachedRoot.empty()) {
        outSourcePath = resolvedCachedRoot;
        return true;
    }

    const fs::path archiveDir = packageCacheFolder() / "Archives";
    std::error_code ec;
    fs::create_directories(archiveDir, ec);
    if (ec) {
        lastError = "Failed to create package cache folder: " + ec.message();
        return false;
    }

    std::string archiveExtension = ".pkg";
    if (!pkg.archiveType.empty()) {
        if (pkg.archiveType == "modupak") archiveExtension = ".modupak";
        else if (pkg.archiveType == "tar.gz" || pkg.archiveType == "tgz") archiveExtension = ".tar.gz";
        else if (pkg.archiveType == "tar") archiveExtension = ".tar";
        else if (pkg.archiveType == "zip") archiveExtension = ".zip";
    } else {
        const fs::path urlPath(pkg.downloadUrl);
        if (urlPath.has_extension()) {
            archiveExtension = urlPath.extension().string();
        }
    }

    const fs::path archivePath = archiveDir / (registryPackageFolderName(pkg).string() + archiveExtension);
    std::string downloadLog;
    if (!downloadFile(pkg.downloadUrl, archivePath, downloadLog)) {
        lastError = "Download failed for " + pkg.id + ". " + downloadLog;
        return false;
    }

    if (!pkg.checksum.empty()) {
        std::string checksumError;
        if (!verifyChecksum(archivePath, pkg.checksum, checksumError)) {
            lastError = "Checksum mismatch for " + pkg.id + ". " + checksumError;
            return false;
        }
    }

    fs::remove_all(extractedRoot, ec);
    fs::create_directories(extractedRoot, ec);
    if (ec) {
        lastError = "Failed to prepare extraction folder: " + ec.message();
        return false;
    }

    std::string extractLog;
    if (!extractArchive(archivePath, pkg.archiveType, extractedRoot, extractLog)) {
        lastError = "Extraction failed for " + pkg.id + ". " + extractLog;
        return false;
    }

    resolvedCachedRoot = resolveExtractedPackageRoot(extractedRoot);
    if (resolvedCachedRoot.empty()) {
        lastError = "Downloaded archive for " + pkg.id + " did not contain a valid package root.";
        return false;
    }

    outSourcePath = resolvedCachedRoot;
    return true;
}

bool PackageManager::extractArchive(const fs::path& archivePath,
                                    const std::string& archiveType,
                                    const fs::path& destinationRoot,
                                    std::string& outLog) const {
    outLog.clear();
    const std::string normalizedType = trimCopy(archiveType);
    std::string command;

    if (normalizedType.empty() || normalizedType == "modupak" || normalizedType == "tar" ||
        normalizedType == "tar.gz" || normalizedType == "tgz") {
        command = "tar -xf \"" + archivePath.string() + "\" -C \"" + destinationRoot.string() + "\" 2>&1";
    } else if (normalizedType == "zip") {
#ifdef _WIN32
        command = "powershell -NoProfile -Command \"Expand-Archive -Force '"
                  + archivePath.string() + "' '" + destinationRoot.string() + "'\" 2>&1";
#else
        command = "unzip -oq \"" + archivePath.string() + "\" -d \"" + destinationRoot.string() + "\" 2>&1";
#endif
    } else {
        outLog = "unsupported archive type: " + normalizedType;
        return false;
    }

    if (!runCommand(command, outLog)) {
        if (outLog.empty()) {
            outLog = "archive extraction command failed";
        }
        return false;
    }
    return true;
}

fs::path PackageManager::resolveExtractedPackageRoot(const fs::path& extractedRoot) const {
    std::error_code ec;
    if (fs::exists(extractedRoot / "Manifest.modu", ec) || fs::exists(extractedRoot / "manifest.modu", ec)) {
        return normalizePath(extractedRoot);
    }
    if (!fs::exists(extractedRoot, ec) || !fs::is_directory(extractedRoot, ec)) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(extractedRoot, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }
        const fs::path candidate = entry.path();
        if (fs::exists(candidate / "Manifest.modu", ec) || fs::exists(candidate / "manifest.modu", ec)) {
            return normalizePath(candidate);
        }
    }
    return {};
}

bool PackageManager::verifyChecksum(const fs::path& filePath, const std::string& checksum, std::string& outError) const {
    outError.clear();
    if (checksum.empty()) {
        return true;
    }

    std::string expected = trim(checksum);
    if (startsWithInsensitive(expected, "sha256:")) {
        expected = expected.substr(7);
    }

    std::string output;
    std::string command;
#ifdef _WIN32
    command = "certutil -hashfile \"" + filePath.string() + "\" SHA256 2>&1";
#else
    command = "sha256sum \"" + filePath.string() + "\" 2>&1";
#endif
    if (!runCommand(command, output)) {
        outError = "failed to compute SHA-256";
        return false;
    }

    std::string actual;
#ifdef _WIN32
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        std::string cleaned = trim(line);
        if (cleaned.empty()) continue;
        if (cleaned.find("SHA256") != std::string::npos) continue;
        if (cleaned.find("CertUtil:") != std::string::npos) continue;
        actual = cleaned;
        break;
    }
#else
    std::istringstream lines(output);
    lines >> actual;
#endif

    actual = trim(actual);
    expected = trim(expected);
    std::transform(actual.begin(), actual.end(), actual.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (actual.empty()) {
        outError = "checksum output was empty";
        return false;
    }
    if (actual != expected) {
        outError = "expected " + expected + " but got " + actual;
        return false;
    }
    return true;
}
#pragma endregion

#pragma region Manifest IO
void PackageManager::loadManifest() {
    installedIds.clear();
    for (const auto& pkg : registry) {
        if (pkg.builtIn && !isInstalled(pkg.id)) {
            installedIds.push_back(pkg.id);
        }
    }

    if (manifestPath.empty()) return;
    if (!fs::exists(manifestPath)) {
        saveManifest();
        return;
    }

    std::ifstream file(manifestPath);
    if (!file.is_open()) {
        lastError = "Unable to open package manifest.";
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string cleaned = trim(line);
        if (cleaned.empty() || cleaned[0] == '#') continue;

        if (cleaned.rfind("package=", 0) == 0) {
            const std::string id = trim(cleaned.substr(8));
            if (!id.empty() && !isInstalled(id) && findPackage(id)) {
                installedIds.push_back(id);
            }
            continue;
        }

        bool isGitLine = false;
        bool isModuPakLine = false;
        std::string payload;
        if (cleaned.rfind("git=", 0) == 0) {
            payload = cleaned.substr(4);
            isGitLine = true;
        } else if (cleaned.rfind("modupak=", 0) == 0) {
            payload = cleaned.substr(8);
            isModuPakLine = true;
        }

        if (!isGitLine && !isModuPakLine) {
            continue;
        }

        PackageInfo pkg;
        if (!parseExternalManifestPackage(projectRoot, payload, isModuPakLine, pkg)) {
            continue;
        }

        registry.erase(std::remove_if(registry.begin(), registry.end(),
            [&](const PackageInfo& entry) { return entry.id == pkg.id && entry.external; }),
            registry.end());
        registry.push_back(pkg);
        if (!isInstalled(pkg.id)) {
            installedIds.push_back(pkg.id);
        }
    }
}

void PackageManager::saveManifest() const {
    if (manifestPath.empty()) return;
    std::ofstream file(manifestPath);
    if (!file.is_open()) return;

    file << "# Modularity package manifest\n";
    file << "# Add optional script-time dependencies here\n";
    file << "# package=<id>\n";
    file << "# git=<id>|<name>|<url>|<path>|<includeDirs>|<defines>|<linuxLibs>|<windowsLibs>|<description>\n";
    file << "# modupak=<id>|<name>|<bundlePath>|<path>|<includeDirs>|<defines>|<linuxLibs>|<windowsLibs>|<description>\n";
    for (const auto& id : installedIds) {
        const PackageInfo* pkg = findPackage(id);
        if (!pkg) continue;
        if (!pkg->external) {
            if (!pkg->builtIn) {
                file << "package=" << id << "\n";
            }
            continue;
        }

        // Persist external package metadata
        std::vector<std::string> relIncludes;
        for (const auto& inc : pkg->includeDirs) {
            std::error_code ec;
            fs::path rel = fs::relative(inc, projectRoot, ec);
            relIncludes.push_back((!ec ? rel : inc).generic_string());
        }

        const char* sourceTag = pkg->modupak ? "modupak=" : "git=";
        file << sourceTag << pkg->id << "|"
             << pkg->name << "|";
        if (pkg->modupak) {
            file << toManifestPathToken(pkg->modupakSourcePath, projectRoot) << "|";
        } else {
            file << pkg->gitUrl << "|";
        }

        file << toManifestPathToken(pkg->localPath, projectRoot) << "|";
        file << join(relIncludes, ';') << "|";
        file << join(pkg->defines, ';') << "|";
        file << join(pkg->linuxLibs, ';') << "|";
        file << join(pkg->windowsLibs, ';') << "|";
        file << pkg->description << "\n";
    }
}
#pragma endregion

#pragma region Registry Lookup
const PackageInfo* PackageManager::findPackage(const std::string& id) const {
    auto it = std::find_if(registry.begin(), registry.end(), [&](const PackageInfo& p) {
        return p.id == id;
    });
    return it == registry.end() ? nullptr : &(*it);
}

bool PackageManager::isBuiltIn(const std::string& id) const {
    const PackageInfo* pkg = findPackage(id);
    return pkg && pkg->builtIn;
}
#pragma endregion

#pragma region Utility Helpers
std::string PackageManager::currentEngineVersion() {
    return "7.0.0";
}

bool PackageManager::isCompatibleVersionString(const std::string& rule, const std::string& currentVersion) {
    const std::string trimmedRule = trim(rule);
    if (trimmedRule.empty()) {
        return true;
    }

    const std::string normalizedRule = normalizeVersionToken(trimmedRule);
    const std::string normalizedCurrent = normalizeVersionToken(currentVersion);
    if (normalizedRule.empty() || normalizedCurrent.empty()) {
        return true;
    }

    auto parseParts = [](const std::string& version) {
        std::vector<int> parts;
        std::stringstream ss(version);
        std::string token;
        while (std::getline(ss, token, '.')) {
            try {
                parts.push_back(std::stoi(token));
            } catch (...) {
                parts.push_back(0);
            }
        }
        return parts;
    };

    std::vector<int> requiredParts = parseParts(normalizedRule);
    std::vector<int> currentParts = parseParts(normalizedCurrent);
    const size_t count = std::max(requiredParts.size(), currentParts.size());
    requiredParts.resize(count, 0);
    currentParts.resize(count, 0);

    for (size_t i = 0; i < count; ++i) {
        if (currentParts[i] > requiredParts[i]) {
            return true;
        }
        if (currentParts[i] < requiredParts[i]) {
            return false;
        }
    }
    return true;
}

std::string PackageManager::trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) start++;
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
    return value.substr(start, end - start);
}

std::string PackageManager::slugify(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (c == '-' || c == '_') {
            out.push_back(c);
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            out.push_back('-');
        }
    }
    if (out.empty()) out = "pkg";
    return out;
}

bool PackageManager::runCommand(const std::string& command, std::string& output) {
#ifdef _WIN32
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
        if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return false;
        }

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;

        std::string commandLine = "cmd.exe /d /s /c \"" + command + "\"";
        std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back('\0');

        PROCESS_INFORMATION pi{};
        const BOOL launched = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr,
                                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(writePipe);
        if (!launched) {
            CloseHandle(readPipe);
            return false;
        }

        char buf[512];
        DWORD read = 0;
        while (ReadFile(readPipe, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
            buf[read] = '\0';
            output += buf;
        }
        CloseHandle(readPipe);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return exitCode == 0;
    }
#else
    std::array<char, 256> buffer{};
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return false;
    const int buffer_len = static_cast<int>(buffer.size());
    while (fgets(buffer.data(), buffer_len, pipe) != nullptr) {
        output += buffer.data();
    }
    return pclose(pipe) == 0;
#endif
}

bool PackageManager::ensureProjectRoot() const {
    return !projectRoot.empty();
}

std::vector<std::string> PackageManager::split(const std::string& input, char delim) const {
    std::vector<std::string> out;
    std::stringstream ss(input);
    std::string part;
    while (std::getline(ss, part, delim)) {
        out.push_back(part);
    }
    return out;
}

std::string PackageManager::join(const std::vector<std::string>& vals, char delim) const {
    std::ostringstream oss;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i > 0) oss << delim;
        oss << vals[i];
    }
    return oss.str();
}
#pragma endregion

#pragma region External Packages
fs::path PackageManager::packagesFolder() const {
    fs::path newFolder = projectRoot / "Library" / "InstalledPackages";
    if (fs::exists(newFolder) || fs::exists(projectRoot / "scripts.modu")) {
        return newFolder;
    }
    return projectRoot / "Packages";
}

bool PackageManager::installGitPackage(const std::string& url,
                                       const std::string& nameHint,
                                       const std::string& includeRel,
                                       std::string& outId) {
    lastError.clear();
    if (!ensureProjectRoot()) {
        lastError = "Project root not set.";
        return false;
    }
    if (url.empty()) {
        lastError = "Git URL is required.";
        return false;
    }

    std::string repoName = nameHint;
    size_t slash = url.find_last_of("/\\");
    if (repoName.empty() && slash != std::string::npos) {
        repoName = url.substr(slash + 1);
        if (repoName.rfind(".git") != std::string::npos) {
            repoName = repoName.substr(0, repoName.size() - 4);
        }
    }
    std::string id = slugify(repoName);
    if (isInstalled(id)) {
        lastError = "Package already installed: " + id;
        return false;
    }

    fs::path dest = normalizePath(packagesFolder() / id);
    fs::create_directories(dest.parent_path());

    std::string cmd;
    if (isGitRepo(projectRoot)) {
        std::error_code ec;
        fs::path relPath = fs::relative(dest, projectRoot, ec);
        std::string rel = (!ec ? relPath : dest).generic_string();
        cmd = "git -C \"" + projectRoot.string() + "\" submodule add --force \"" + url + "\" \"" + rel + "\"";
    } else {
        cmd = "git clone \"" + url + "\" \"" + dest.string() + "\"";
    }
    std::string log;
    if (!runCommand(cmd, log)) {
        if (isGitRepo(projectRoot)) {
            lastError = "git submodule add failed:\n" + log;
        } else {
            lastError = "git clone failed:\n" + log;
        }
        return false;
    }

    PackageInfo pkg;
    pkg.id = id;
    pkg.name = repoName.empty() ? id : repoName;
    pkg.description = "External package from " + url;
    pkg.external = true;
    pkg.gitUrl = url;
    pkg.localPath = dest;
    pkg.includeDirs.push_back(guessIncludeDir(dest, includeRel));

    registry.push_back(pkg);
    installedIds.push_back(id);
    saveManifest();
    outId = id;
    return true;
}

bool PackageManager::installModuPak(const fs::path& modupakPath, std::string& outId) {
    lastError.clear();
    outId.clear();

    if (!ensureProjectRoot()) {
        lastError = "Project root not set.";
        return false;
    }
    if (modupakPath.empty()) {
        lastError = ".modupak path is required.";
        return false;
    }

    const fs::path source = normalizePath(modupakPath);
    if (!fs::exists(source)) {
        lastError = ".modupak was not found: " + source.string();
        return false;
    }
    if (source.extension() != ".modupak") {
        lastError = "Expected a .modupak bundle.";
        return false;
    }

    const fs::path tempRoot = projectRoot / "Library" / "Temp" / "ModuPakInstall";
    std::error_code ec;
    fs::create_directories(tempRoot, ec);
    if (ec) {
        lastError = "Failed to create temp folder: " + ec.message();
        return false;
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path unpackRoot = tempRoot / ("extract_" + std::to_string(now));
    fs::create_directories(unpackRoot, ec);
    if (ec) {
        lastError = "Failed to create extraction folder: " + ec.message();
        return false;
    }

    auto cleanup = [&]() {
        std::error_code removeEc;
        fs::remove_all(unpackRoot, removeEc);
    };

    if (fs::is_directory(source)) {
        std::string copyError;
        if (!copyDirectoryRecursive(source, unpackRoot, copyError)) {
            cleanup();
            lastError = copyError;
            return false;
        }
    } else {
        std::string tarLog;
        const std::string extractCmd =
            "tar -xf \"" + source.string() + "\" -C \"" + unpackRoot.string() + "\" 2>&1";
        if (!runCommand(extractCmd, tarLog)) {
            cleanup();
            lastError = "Failed to extract .modupak with tar.\n" + tarLog;
            return false;
        }
    }

    const fs::path manifestFile = unpackRoot / "manifest.modu";
    if (!fs::exists(manifestFile)) {
        cleanup();
        lastError = ".modupak is missing manifest.modu.";
        return false;
    }

    PackageInfo pkg;
    pkg.external = true;
    pkg.modupak = true;
    pkg.modupakSourcePath = source;
    pkg.description = "External package from .modupak";

    std::vector<std::string> includeHints;
    std::ifstream manifest(manifestFile);
    if (!manifest.is_open()) {
        cleanup();
        lastError = "Failed to open .modupak manifest.";
        return false;
    }
    std::string line;
    while (std::getline(manifest, line)) {
        const std::string cleaned = trim(line);
        if (cleaned.empty() || cleaned[0] == '#') continue;

        auto readValue = [&](size_t prefixSize) {
            return trim(cleaned.substr(prefixSize));
        };

        if (cleaned.rfind("id=", 0) == 0) {
            pkg.id = slugify(readValue(3));
        } else if (cleaned.rfind("name=", 0) == 0) {
            pkg.name = readValue(5);
        } else if (cleaned.rfind("description=", 0) == 0) {
            pkg.description = readValue(12);
        } else if (cleaned.rfind("includeDir=", 0) == 0) {
            const std::string value = readValue(11);
            if (!value.empty()) includeHints.push_back(value);
        } else if (cleaned.rfind("define=", 0) == 0) {
            const std::string value = readValue(7);
            if (!value.empty()) pkg.defines.push_back(value);
        } else if (cleaned.rfind("linux.linkLib=", 0) == 0) {
            const std::string value = readValue(14);
            if (!value.empty()) pkg.linuxLibs.push_back(value);
        } else if (cleaned.rfind("win.linkLib=", 0) == 0) {
            const std::string value = readValue(12);
            if (!value.empty()) pkg.windowsLibs.push_back(value);
        } else if (cleaned.rfind("windows.linkLib=", 0) == 0) {
            const std::string value = readValue(16);
            if (!value.empty()) pkg.windowsLibs.push_back(value);
        }
    }

    if (pkg.name.empty()) {
        pkg.name = source.stem().string();
    }
    if (pkg.id.empty()) {
        pkg.id = slugify(pkg.name);
    }
    if (pkg.id.empty()) {
        cleanup();
        lastError = "Unable to resolve package id from .modupak.";
        return false;
    }
    if (isInstalled(pkg.id)) {
        cleanup();
        lastError = "Package already installed: " + pkg.id;
        return false;
    }

    pkg.localPath = normalizePath(packagesFolder() / pkg.id);
    if (fs::exists(pkg.localPath)) {
        cleanup();
        lastError = "Package target path already exists: " + pkg.localPath.string();
        return false;
    }

    const fs::path payloadRoot = unpackRoot / "payload";
    const fs::path sourceContent =
        (fs::exists(payloadRoot) && fs::is_directory(payloadRoot)) ? payloadRoot : unpackRoot;

    std::string installCopyError;
    if (!copyDirectoryRecursive(sourceContent, pkg.localPath, installCopyError)) {
        cleanup();
        lastError = installCopyError;
        return false;
    }

    for (const std::string& includeRel : includeHints) {
        pkg.includeDirs.push_back(normalizePath(pkg.localPath / includeRel));
    }
    if (pkg.includeDirs.empty()) {
        pkg.includeDirs.push_back(guessIncludeDir(pkg.localPath, "include"));
    }

    registry.push_back(pkg);
    installedIds.push_back(pkg.id);
    saveManifest();
    outId = pkg.id;
    cleanup();
    return true;
}

bool PackageManager::checkGitStatus(const std::string& id, std::string& outStatus) {
    lastError.clear();
    outStatus.clear();
    const PackageInfo* pkg = findPackage(id);
    if (!pkg || !pkg->external || pkg->gitUrl.empty()) {
        lastError = "Package is not a Git package or was not found.";
        return false;
    }

    std::string fetchCmd = "git -C \"" + pkg->localPath.string() + "\" fetch --quiet";
    runCommand(fetchCmd, outStatus); // ignore fetch failures here

    std::string statusCmd = "git -C \"" + pkg->localPath.string() + "\" status -sb";
    if (!runCommand(statusCmd, outStatus)) {
        lastError = "Failed to read git status.";
        return false;
    }
    return true;
}

bool PackageManager::updateGitPackage(const std::string& id, std::string& outLog) {
    lastError.clear();
    outLog.clear();
    const PackageInfo* pkg = findPackage(id);
    if (!pkg || !pkg->external || pkg->gitUrl.empty()) {
        lastError = "Package is not a Git package or was not found.";
        return false;
    }
    std::string cmd = "git -C \"" + pkg->localPath.string() + "\" pull --ff-only";
    if (!runCommand(cmd, outLog)) {
        lastError = "git pull failed.";
        return false;
    }
    return true;
}
#pragma endregion
