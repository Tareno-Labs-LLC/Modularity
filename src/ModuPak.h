#pragma once

#include "Common.h"
#include "SceneObject.h"

struct ModuPakManifest {
    std::string packageID;
    std::string name;
    std::string author;
    std::string description;
    std::string packageType = "Standalone Package";
    std::string subsystem = "ModuPAK";
    std::string version = "1.0.0";
    std::string compatibleModuEngineVersion = "6.8+";

    std::string toText() const;
    static bool parse(const std::string& text, ModuPakManifest& outManifest);
};

struct ModuPakFileEntry {
    fs::path sourcePath;
    fs::path archivePath;
    uintmax_t sizeBytes = 0;
    bool selected = true;
    bool isDirectory = false;
    bool existsInProject = false;
    std::string statusTag = "NEW";
};

struct ModuPakExportOptions {
    fs::path projectRoot;
    fs::path outputPath;
    ModuPakManifest manifest;
    std::vector<fs::path> inputPaths;
    bool recursiveFolders = true;
};

struct ModuPakExportResult {
    bool success = false;
    std::string message;
    size_t fileCount = 0;
    uintmax_t packageSizeBytes = 0;
};

struct ModuPakImportPreview {
    bool success = false;
    std::string message;
    ModuPakManifest manifest;
    std::vector<ModuPakFileEntry> entries;
};

struct ModuPakImportOptions {
    fs::path projectRoot;
    fs::path packagePath;
    std::vector<ModuPakFileEntry> entries;
};

struct ModuPakImportResult {
    bool success = false;
    std::string message;
    size_t importedCount = 0;
    size_t replacedCount = 0;
    size_t skippedCount = 0;
};

class ModuPakExporter {
public:
    static std::string makePackageID(const std::string& name, const fs::path& outputPath = {});
    static std::vector<ModuPakFileEntry> collectEntries(const fs::path& projectRoot,
                                                        const std::vector<fs::path>& inputPaths,
                                                        bool recursiveFolders,
                                                        std::string& outError);
    static bool exportPackage(const ModuPakExportOptions& options, ModuPakExportResult& outResult);

private:
    static fs::path classifyArchivePath(const fs::path& projectRoot, const fs::path& sourcePath);
};

class ModuPakImporter {
public:
    static bool readPreview(const fs::path& packagePath,
                            const fs::path& projectRoot,
                            ModuPakImportPreview& outPreview);
    static bool importPackage(const ModuPakImportOptions& options, ModuPakImportResult& outResult);
    static bool isSafeArchivePath(const fs::path& archivePath, std::string* outReason = nullptr);
};

class ModuObjExporter {
public:
    static bool exportObject(const fs::path& projectRoot,
                             const fs::path& outputPath,
                             const ModuPakManifest& manifest,
                             const std::vector<SceneObject>& sceneObjects,
                             const std::vector<int>& selectedObjectIds,
                             int nextObjectId,
                             ModuPakExportResult& outResult);
};

class ModuObjImporter {
public:
    static bool importObject(const fs::path& projectRoot,
                             const fs::path& packagePath,
                             std::vector<SceneObject>& sceneObjects,
                             int& nextObjectId,
                             std::vector<int>* outImportedIds,
                             ModuPakImportResult& outResult);
};
