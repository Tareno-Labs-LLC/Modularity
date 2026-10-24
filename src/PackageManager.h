#pragma once

#include "Common.h"
#include "ScriptCompiler.h"
#include <cctype>
#include <unordered_map>

struct PackageInfo {
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    std::string packageType;
    std::string subsystem;
    std::string version;
    std::string compatibleModuEngineVersion;
    std::string registryEngineVersion;
    std::string downloadUrl;
    std::string archiveType;
    std::string checksum;
    bool builtIn = false;
    bool external = false;
    bool modupak = false;
    bool registryPackage = false;
    std::string gitUrl;
    fs::path modupakSourcePath;
    fs::path localPath;     // Absolute path for external packages
    fs::path includeHint;   // Absolute include root for external packages
    fs::path registrySourcePath;
    std::vector<fs::path> includeDirs;
    std::vector<std::string> defines;
    std::vector<std::string> linuxLibs;
    std::vector<std::string> windowsLibs;
};

// Minimal package manager for script dependencies. Keeps a small registry of
// known/bundled packages and a per-project manifest of which optional ones are
// enabled. Installed packages augment the script build config (include dirs,
// defines, link libs) before compilation.
class PackageManager {
public:
    PackageManager();

    void setProjectRoot(const fs::path& root);
    void refreshRegistry();
    const std::vector<PackageInfo>& getRegistry() const { return registry; }
    const std::vector<std::string>& getInstalled() const { return installedIds; }
    bool isInstalled(const std::string& id) const;
    bool hasProjectInstallPayload(const std::string& id) const;
    bool isGloballyInstalled(const std::string& id) const;
    bool hasUpdateAvailable(const std::string& id) const;
    bool isCompatible(const std::string& id) const;
    bool isCompatible(const PackageInfo& pkg) const;
    bool install(const std::string& id);
    bool installRegistryPackageToProject(const std::string& id);
    bool installRegistryPackageGlobally(const std::string& id);
    bool installGitPackage(const std::string& url,
                           const std::string& nameHint,
                           const std::string& includeRel,
                           std::string& outId);
    bool installModuPak(const fs::path& modupakPath, std::string& outId);
    bool checkGitStatus(const std::string& id, std::string& outStatus);
    bool updateGitPackage(const std::string& id, std::string& outLog);
    bool remove(const std::string& id);
    bool removeRegistryPackageGlobally(const std::string& id);
    void applyToBuildConfig(ScriptBuildConfig& config) const;
    const std::string& getLastError() const { return lastError; }
    bool hasRegistryMetadata() const { return registryAvailable; }
    const std::string& getRegistryStatus() const { return registryStatus; }
    const std::string& getRegistryLastUpdated() const { return registryLastUpdated; }
    const std::string& getRegistryUpdatedBy() const { return registryUpdatedBy; }

    struct RegistryPackageLocations {
        fs::path source;
        fs::path destination;
        std::string downloadUrl;
    };
    bool resolveRegistryPackageLocations(const std::string& id, RegistryPackageLocations& out) const;

    static std::string currentEngineVersion();
    fs::path globalPackagesRoot() const { return globalPackagesFolder(); }

private:
    bool allowRemoteRegistryFetch = false;
    void buildRegistry();
    void loadManifest();
    void saveManifest() const;
    bool loadRegistryMetadata(const fs::path& engineRoot);
    fs::path findRegistryRoot(const fs::path& start) const;
    fs::path resolveRegistrySourcePath(const PackageInfo& pkg) const;
    fs::path projectRegistryPackagePath(const PackageInfo& pkg) const;
    fs::path globalPackagesFolder() const;
    fs::path packageCacheFolder() const;
    fs::path globalRegistryPackagePath(const PackageInfo& pkg) const;
    fs::path registryPackageFolderName(const PackageInfo& pkg) const;
    bool hasVersionedPackageAt(const fs::path& root, const PackageInfo& pkg) const;
    bool hasLegacyVersionedPackageAt(const fs::path& root, const PackageInfo& pkg) const;
    fs::path registryPackageCachePath(const PackageInfo& pkg) const;
    bool downloadRemoteRegistryListing(fs::path& outListingPath, std::string& outLog) const;
    bool downloadFile(const std::string& url, const fs::path& destination, std::string& outLog) const;
    bool downloadRegistryPackageToCache(const PackageInfo& pkg, fs::path& outSourcePath);
    bool extractArchive(const fs::path& archivePath, const std::string& archiveType, const fs::path& destinationRoot, std::string& outLog) const;
    fs::path resolveExtractedPackageRoot(const fs::path& extractedRoot) const;
    bool verifyChecksum(const fs::path& filePath, const std::string& checksum, std::string& outError) const;
    std::string configuredRegistryBaseUrl() const;
    std::string configuredRegistryMetadataUrl() const;
    const PackageInfo* findPackage(const std::string& id) const;
    bool isBuiltIn(const std::string& id) const;
    static bool isCompatibleVersionString(const std::string& rule, const std::string& currentVersion);
    static std::string trim(const std::string& value);
    static std::string slugify(const std::string& value);
    static bool runCommand(const std::string& command, std::string& output);
    bool ensureProjectRoot() const;
    void invalidateInstallationStateCache();
    std::vector<std::string> split(const std::string& input, char delim) const;
    std::string join(const std::vector<std::string>& vals, char delim) const;
    fs::path packagesFolder() const;

    fs::path projectRoot;
    fs::path manifestPath;
    std::vector<PackageInfo> registry;
    std::vector<std::string> installedIds;
    std::string lastError;
    bool registryAvailable = false;
    std::string registryStatus;
    std::string registryLastUpdated;
    std::string registryUpdatedBy;
    fs::path registryRoot;
    mutable std::unordered_map<std::string, bool> projectPayloadCache;
    mutable std::unordered_map<std::string, bool> globalInstallCache;
    mutable std::unordered_map<std::string, bool> updateAvailableCache;
};
