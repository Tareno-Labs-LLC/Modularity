#include "ModuPak.h"
#include "ProjectManager.h"
#include "../include/Platform/AssetSource.h"
#include "ThirdParty/assimp/contrib/unzip/unzip.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <unordered_map>

namespace {
constexpr const char* kManifestName = "Manifest.modu";
constexpr const char* kObjectSceneName = "Object.scene";
constexpr const char* kEngineCompatibility = "6.8+";

std::string quotePath(const fs::path& path) {
#ifdef _WIN32
    std::string s = path.string();
    size_t pos = 0;
    while ((pos = s.find('"', pos)) != std::string::npos) {
        s.insert(pos, "\\");
        pos += 2;
    }
    return "\"" + s + "\"";
#else
    std::string s = path.string();
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

std::string trimCopy(std::string value) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string escapeManifestString(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n' || c == '\r') out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

fs::path normalizeRelativePath(fs::path path) {
    fs::path normalized = path.lexically_normal();
    fs::path clean;
    for (const auto& part : normalized) {
        if (part == ".") continue;
        clean /= part;
    }
    return clean;
}

bool pathStartsWith(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    fs::path absPath = fs::weakly_canonical(path, ec);
    if (ec) absPath = fs::absolute(path).lexically_normal();
    fs::path absRoot = fs::weakly_canonical(root, ec);
    if (ec) absRoot = fs::absolute(root).lexically_normal();
    auto pIt = absPath.begin();
    auto rIt = absRoot.begin();
    for (; rIt != absRoot.end(); ++rIt, ++pIt) {
        if (pIt == absPath.end() || *pIt != *rIt) return false;
    }
    return true;
}

fs::path makeTempDirectory(const std::string& prefix) {
    fs::path base = fs::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
        fs::path candidate = base / (prefix + "_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(i));
        std::error_code ec;
        if (fs::create_directories(candidate, ec)) return candidate;
    }
    return {};
}

bool copyFileChecked(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (ec) {
        error = "Failed to create folder: " + to.parent_path().string() + " (" + ec.message() + ")";
        return false;
    }
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Failed to copy " + from.string() + " to " + to.string() + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

bool runZipCreate(const fs::path& stagingRoot, const fs::path& outputPath, std::string& error) {
    std::error_code ec;
    fs::create_directories(outputPath.parent_path(), ec);
    fs::remove(outputPath, ec);
#ifdef _WIN32
    std::string command = "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Compress-Archive -Path " +
                          quotePath(stagingRoot / "*") + " -DestinationPath " + quotePath(outputPath) + " -Force\"";
#else
    std::string command = "cd " + quotePath(stagingRoot) + " && zip -X -q -r " + quotePath(outputPath) + " .";
#endif
    int rc = std::system(command.c_str());
    if (rc != 0 || !fs::exists(outputPath)) {
        error = "Failed to create archive. Ensure zip/PowerShell archive support is available.";
        return false;
    }
    return true;
}

bool runZipExtract(const fs::path& archivePath, const fs::path& destination, std::string& error) {
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        error = "Failed to create folder: " + destination.string() + " (" + ec.message() + ")";
        return false;
    }

    fs::path stagedArchive = makeTempDirectory("modupak_archive");
    if (stagedArchive.empty()) {
        error = "Failed to create temporary archive directory.";
        return false;
    }
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; if (!path.empty()) fs::remove_all(path, ec); } } cleanup{stagedArchive};
    stagedArchive /= archivePath.filename().empty() ? fs::path("package.modupak") : archivePath.filename();

    {
        std::ofstream out(stagedArchive, std::ios::binary);
        if (!out) {
            error = "Failed to stage ModuPAK archive: " + stagedArchive.string();
            return false;
        }

        std::array<char, 64 * 1024> buffer{};
        auto stream = Modularity::Platform::GetAssetSource().Open(archivePath.generic_string());
        if (stream) {
            uint64_t totalRead = 0;
            for (;;) {
                const size_t bytesRead = stream->Read(buffer.data(), buffer.size());
                if (bytesRead == 0) break;
                out.write(buffer.data(), static_cast<std::streamsize>(bytesRead));
                if (!out) {
                    error = "Failed to write staged ModuPAK archive: " + stagedArchive.string();
                    return false;
                }
                totalRead += bytesRead;
            }

            const int64_t expectedSize = stream->Size();
            if (expectedSize >= 0 && totalRead != static_cast<uint64_t>(expectedSize)) {
                error = "Failed to read complete ModuPAK archive: " + archivePath.string();
                return false;
            }
        } else {
            std::ifstream in(archivePath, std::ios::binary);
            if (!in) {
                error = "Failed to open ModuPAK archive: " + archivePath.string();
                return false;
            }
            for (;;) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize bytesRead = in.gcount();
                if (bytesRead > 0) {
                    out.write(buffer.data(), bytesRead);
                    if (!out) {
                        error = "Failed to write staged ModuPAK archive: " + stagedArchive.string();
                        return false;
                    }
                }
                if (in.eof()) break;
                if (!in) {
                    error = "Failed to read ModuPAK archive: " + archivePath.string();
                    return false;
                }
            }
        }
    }

    unzFile zip = unzOpen64(stagedArchive.string().c_str());
    if (!zip) {
        error = "Failed to open ModuPAK zip data: " + archivePath.string();
        return false;
    }
    struct ZipCleanup { unzFile zip; ~ZipCleanup() { if (zip) unzClose(zip); } } zipCleanup{zip};

    int rc = unzGoToFirstFile(zip);
    if (rc == UNZ_END_OF_LIST_OF_FILE) return true;
    if (rc != UNZ_OK) {
        error = "Failed to read ModuPAK archive entries: " + archivePath.string();
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while (rc == UNZ_OK) {
        unz_file_info64 info{};
        std::vector<char> nameBuffer(512);
        int infoRc = UNZ_OK;
        for (;;) {
            infoRc = unzGetCurrentFileInfo64(zip, &info, nameBuffer.data(),
                                             static_cast<uLong>(nameBuffer.size()),
                                             nullptr, 0, nullptr, 0);
            if (infoRc != UNZ_OK) {
                error = "Failed to read ModuPAK archive entry info.";
                return false;
            }
            if (info.size_filename < nameBuffer.size()) break;
            nameBuffer.assign(static_cast<size_t>(info.size_filename) + 1, '\0');
        }

        std::string entryName(nameBuffer.data());
        std::replace(entryName.begin(), entryName.end(), '\\', '/');
        fs::path rel = normalizeRelativePath(fs::path(entryName));
        std::string reason;
        if (!ModuPakImporter::isSafeArchivePath(rel, &reason)) {
            error = "Rejected package: " + reason;
            return false;
        }

        const bool isDirectory = !entryName.empty() && entryName.back() == '/';
        fs::path outputPath = destination / rel;
        if (isDirectory) {
            fs::create_directories(outputPath, ec);
            if (ec) {
                error = "Failed to create folder: " + outputPath.string() + " (" + ec.message() + ")";
                return false;
            }
        } else {
            fs::create_directories(outputPath.parent_path(), ec);
            if (ec) {
                error = "Failed to create folder: " + outputPath.parent_path().string() + " (" + ec.message() + ")";
                return false;
            }

            if (unzOpenCurrentFile(zip) != UNZ_OK) {
                error = "Failed to open ModuPAK archive entry: " + rel.generic_string();
                return false;
            }

            std::ofstream out(outputPath, std::ios::binary);
            if (!out) {
                unzCloseCurrentFile(zip);
                error = "Failed to create extracted file: " + outputPath.string();
                return false;
            }

            for (;;) {
                const int bytesRead = unzReadCurrentFile(zip, buffer.data(),
                                                         static_cast<unsigned int>(buffer.size()));
                if (bytesRead < 0) {
                    unzCloseCurrentFile(zip);
                    error = "Failed to read ModuPAK archive entry: " + rel.generic_string();
                    return false;
                }
                if (bytesRead == 0) break;
                out.write(buffer.data(), bytesRead);
                if (!out) {
                    unzCloseCurrentFile(zip);
                    error = "Failed to write extracted file: " + outputPath.string();
                    return false;
                }
            }

            const int closeRc = unzCloseCurrentFile(zip);
            if (closeRc != UNZ_OK) {
                error = "Failed to verify ModuPAK archive entry: " + rel.generic_string();
                return false;
            }
        }

        rc = unzGoToNextFile(zip);
    }
    if (rc != UNZ_END_OF_LIST_OF_FILE) {
        error = "Failed while walking ModuPAK archive entries: " + archivePath.string();
        return false;
    }
    return true;
}

std::vector<fs::path> listExtractedFiles(const fs::path& root) {
    std::vector<fs::path> files;
    if (!fs::exists(root)) return files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::error_code ec;
        fs::path rel = fs::relative(entry.path(), root, ec);
        if (!ec) files.push_back(normalizeRelativePath(rel));
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return a.generic_string() < b.generic_string();
    });
    return files;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeTextFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

bool isScriptExt(const std::string& ext) {
    static const std::set<std::string> exts{".moducpp", ".cpp", ".c", ".cs", ".h", ".hpp"};
    return exts.count(lowerCopy(ext)) > 0;
}

bool isBinaryExt(const std::string& ext) {
    static const std::set<std::string> exts{".so", ".dll", ".dylib"};
    return exts.count(lowerCopy(ext)) > 0;
}

bool isEditorResourcePath(const fs::path& path) {
    std::string lower = lowerCopy(path.generic_string());
    return lower.find("editor") != std::string::npos ||
           lower.find("icon") != std::string::npos ||
           lower.find("thumbnail") != std::string::npos;
}

void collectDependency(std::set<fs::path>& deps, const fs::path& projectRoot, const std::string& rawPath) {
    if (rawPath.empty()) return;
    fs::path path(rawPath);
    if (!path.is_absolute()) path = projectRoot / path;
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec) || !pathStartsWith(path, projectRoot)) return;
    deps.insert(fs::weakly_canonical(path, ec));
}

std::vector<SceneObject> collectObjectHierarchy(const std::vector<SceneObject>& objects,
                                                const std::vector<int>& selectedIds) {
    std::unordered_map<int, const SceneObject*> byId;
    for (const SceneObject& obj : objects) byId[obj.id] = &obj;
    std::set<int> wanted;
    std::vector<int> stack = selectedIds;
    while (!stack.empty()) {
        int id = stack.back();
        stack.pop_back();
        if (!wanted.insert(id).second) continue;
        auto it = byId.find(id);
        if (it == byId.end()) continue;
        for (int childId : it->second->childIds) stack.push_back(childId);
    }
    std::vector<SceneObject> result;
    for (const SceneObject& obj : objects) {
        if (wanted.count(obj.id) == 0) continue;
        SceneObject copy = obj;
        if (copy.parentId != -1 && wanted.count(copy.parentId) == 0) {
            copy.parentId = -1;
        }
        copy.childIds.erase(std::remove_if(copy.childIds.begin(), copy.childIds.end(),
                          [&](int id) { return wanted.count(id) == 0; }),
                          copy.childIds.end());
        result.push_back(std::move(copy));
    }
    return result;
}

void remapImportedObjects(std::vector<SceneObject>& imported,
                          int& nextObjectId,
                          std::vector<int>* outImportedIds) {
    std::unordered_map<int, int> idMap;
    for (SceneObject& obj : imported) {
        int oldId = obj.id;
        int newId = nextObjectId++;
        idMap[oldId] = newId;
        obj.id = newId;
        if (outImportedIds) outImportedIds->push_back(newId);
    }
    for (SceneObject& obj : imported) {
        obj.parentId = idMap.count(obj.parentId) ? idMap[obj.parentId] : -1;
        for (int& childId : obj.childIds) {
            childId = idMap.count(childId) ? idMap[childId] : -1;
        }
        obj.childIds.erase(std::remove(obj.childIds.begin(), obj.childIds.end(), -1), obj.childIds.end());
    }
}
}

std::string ModuPakManifest::toText() const {
    std::ostringstream out;
    out << "ManifestInfo()\n";
    out << "{\n";
    out << "    PackageID = \"" << escapeManifestString(packageID) << "\";\n";
    out << "    Name = \"" << escapeManifestString(name) << "\";\n";
    out << "    Author = \"" << escapeManifestString(author) << "\";\n";
    out << "    Description = \"" << escapeManifestString(description) << "\";\n";
    out << "    PackageType = \"" << escapeManifestString(packageType) << "\";\n";
    out << "    Subsystem = \"" << escapeManifestString(subsystem) << "\";\n";
    out << "    Version = \"" << escapeManifestString(version.empty() ? "1.0.0" : version) << "\";\n";
    out << "    CompatibleModuEngineVersion = \"" << escapeManifestString(compatibleModuEngineVersion.empty() ? kEngineCompatibility : compatibleModuEngineVersion) << "\";\n";
    out << "}\n";
    return out.str();
}

bool ModuPakManifest::parse(const std::string& text, ModuPakManifest& outManifest) {
    std::regex fieldRegex("([A-Za-z0-9_]+)\\s*=\\s*\"((?:\\\\\"|[^\"])*)\"\\s*;");
    bool any = false;
    for (std::sregex_iterator it(text.begin(), text.end(), fieldRegex), end; it != end; ++it) {
        any = true;
        std::string key = (*it)[1].str();
        std::string value = (*it)[2].str();
        if (key == "PackageID") outManifest.packageID = value;
        else if (key == "Name") outManifest.name = value;
        else if (key == "Author") outManifest.author = value;
        else if (key == "Description") outManifest.description = value;
        else if (key == "PackageType") outManifest.packageType = value;
        else if (key == "Subsystem") outManifest.subsystem = value;
        else if (key == "Version") outManifest.version = value;
        else if (key == "CompatibleModuEngineVersion") outManifest.compatibleModuEngineVersion = value;
    }
    return any && !outManifest.packageID.empty();
}

std::string ModuPakExporter::makePackageID(const std::string& name, const fs::path& outputPath) {
    std::string trimmed = trimCopy(name);
    std::string id;
    bool lastSep = false;
    for (unsigned char c : trimmed) {
        if (std::isalnum(c)) {
            id.push_back(static_cast<char>(std::tolower(c)));
            lastSep = false;
        } else if (std::isspace(c) || c == '-' || c == '_') {
            if (!lastSep && !id.empty()) id.push_back('_');
            lastSep = true;
        }
    }
    while (!id.empty() && id.back() == '_') id.pop_back();
    if (id.empty()) id = "modularity_package";
    if (!outputPath.empty() && fs::exists(outputPath)) {
        id += "_" + std::to_string(std::hash<std::string>{}(outputPath.string()) & 0xffff);
    }
    return id;
}

fs::path ModuPakExporter::classifyArchivePath(const fs::path& projectRoot, const fs::path& sourcePath) {
    fs::path rel;
    std::error_code ec;
    rel = fs::relative(sourcePath, projectRoot, ec);
    if (ec || rel.empty()) rel = sourcePath.filename();
    rel = normalizeRelativePath(rel);
    const std::string ext = sourcePath.extension().string();
    if (isBinaryExt(ext)) return fs::path("Pre-Compiled Scripts") / sourcePath.filename();
    if (isScriptExt(ext)) return fs::path("Scripts") / sourcePath.filename();
    if (isEditorResourcePath(rel)) return fs::path("Editor") / "Resources" / sourcePath.filename();
    return rel;
}

std::vector<ModuPakFileEntry> ModuPakExporter::collectEntries(const fs::path& projectRoot,
                                                              const std::vector<fs::path>& inputPaths,
                                                              bool recursiveFolders,
                                                              std::string& outError) {
    std::unordered_map<std::string, ModuPakFileEntry> unique;
    for (const fs::path& input : inputPaths) {
        if (input.empty()) continue;
        fs::path path = input.is_absolute() ? input : projectRoot / input;
        std::error_code ec;
        if (!fs::exists(path, ec)) continue;
        if (!pathStartsWith(path, projectRoot)) {
            outError = "Cannot export path outside project: " + path.string();
            return {};
        }
        auto addFile = [&](const fs::path& filePath) {
            ModuPakFileEntry entry;
            entry.sourcePath = filePath;
            entry.archivePath = classifyArchivePath(projectRoot, filePath);
            entry.sizeBytes = fs::file_size(filePath, ec);
            unique[entry.archivePath.generic_string()] = entry;
        };
        if (fs::is_directory(path, ec)) {
            if (recursiveFolders) {
                for (const auto& child : fs::recursive_directory_iterator(path)) {
                    if (child.is_regular_file()) addFile(child.path());
                }
            }
        } else if (fs::is_regular_file(path, ec)) {
            if (path.filename() != kManifestName) addFile(path);
        }
    }
    std::vector<ModuPakFileEntry> entries;
    entries.reserve(unique.size());
    for (auto& pair : unique) entries.push_back(std::move(pair.second));
    std::sort(entries.begin(), entries.end(), [](const ModuPakFileEntry& a, const ModuPakFileEntry& b) {
        return a.archivePath.generic_string() < b.archivePath.generic_string();
    });
    return entries;
}

bool ModuPakExporter::exportPackage(const ModuPakExportOptions& options, ModuPakExportResult& outResult) {
    outResult = {};
    if (options.projectRoot.empty() || options.outputPath.empty()) {
        outResult.message = "Project root and output path are required.";
        return false;
    }
    std::string error;
    std::vector<ModuPakFileEntry> entries = collectEntries(options.projectRoot, options.inputPaths, options.recursiveFolders, error);
    if (!error.empty()) {
        outResult.message = error;
        return false;
    }
    fs::path staging = makeTempDirectory("modupak_export");
    if (staging.empty()) {
        outResult.message = "Failed to create temporary export directory.";
        return false;
    }
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; if (!path.empty()) fs::remove_all(path, ec); } } cleanup{staging};
    if (!writeTextFile(staging / kManifestName, options.manifest.toText())) {
        outResult.message = "Failed to write package manifest.";
        return false;
    }
    for (const ModuPakFileEntry& entry : entries) {
        if (!entry.selected) continue;
        if (!copyFileChecked(entry.sourcePath, staging / entry.archivePath, outResult.message)) return false;
        outResult.fileCount++;
    }
    if (!runZipCreate(staging, options.outputPath, outResult.message)) return false;
    std::error_code ec;
    outResult.packageSizeBytes = fs::file_size(options.outputPath, ec);
    outResult.success = true;
    outResult.message = "Exported ModuPAK.";
    return true;
}

bool ModuPakImporter::isSafeArchivePath(const fs::path& archivePath, std::string* outReason) {
    if (archivePath.empty() || archivePath.is_absolute()) {
        if (outReason) *outReason = "absolute or empty archive path";
        return false;
    }
    fs::path normalized = archivePath.lexically_normal();
    for (const auto& part : normalized) {
        std::string token = part.string();
        if (token == ".." || token.find('\0') != std::string::npos ||
            token.find(':') != std::string::npos || token == "." || token.empty()) {
            if (outReason) *outReason = "unsafe archive path: " + archivePath.generic_string();
            return false;
        }
    }
    return true;
}

bool ModuPakImporter::readPreview(const fs::path& packagePath,
                                  const fs::path& projectRoot,
                                  ModuPakImportPreview& outPreview) {
    outPreview = {};
    fs::path staging = makeTempDirectory("modupak_preview");
    if (staging.empty()) {
        outPreview.message = "Failed to create temporary preview directory.";
        return false;
    }
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; if (!path.empty()) fs::remove_all(path, ec); } } cleanup{staging};
    if (!runZipExtract(packagePath, staging, outPreview.message)) return false;
    std::string manifestText = readTextFile(staging / kManifestName);
    ModuPakManifest::parse(manifestText, outPreview.manifest);
    std::vector<fs::path> files = listExtractedFiles(staging);
    for (const fs::path& rel : files) {
        std::string reason;
        if (!isSafeArchivePath(rel, &reason)) {
            outPreview.message = "Rejected package: " + reason;
            return false;
        }
        ModuPakFileEntry entry;
        entry.archivePath = rel;
        entry.sourcePath = staging / rel;
        std::error_code ec;
        entry.sizeBytes = fs::file_size(entry.sourcePath, ec);
        entry.existsInProject = fs::exists(projectRoot / rel, ec);
        entry.statusTag = rel.filename() == kManifestName ? "SKIP" : (entry.existsInProject ? "REPLACE" : "NEW");
        entry.selected = rel.filename() != kManifestName;
        outPreview.entries.push_back(entry);
    }
    auto priority = [](const fs::path& p) {
        const std::string s = p.generic_string();
        if (s == "Editor") return 0;
        if (s.rfind("Editor/Resources", 0) == 0) return 1;
        if (s.rfind("Pre-Compiled Scripts", 0) == 0) return 2;
        if (s.rfind("Scripts", 0) == 0) return 3;
        if (s == kManifestName) return 4;
        return 5;
    };
    std::sort(outPreview.entries.begin(), outPreview.entries.end(), [&](const ModuPakFileEntry& a, const ModuPakFileEntry& b) {
        int pa = priority(a.archivePath);
        int pb = priority(b.archivePath);
        if (pa != pb) return pa < pb;
        return a.archivePath.generic_string() < b.archivePath.generic_string();
    });
    outPreview.success = true;
    return true;
}

bool ModuPakImporter::importPackage(const ModuPakImportOptions& options, ModuPakImportResult& outResult) {
    outResult = {};
    fs::path staging = makeTempDirectory("modupak_import");
    if (staging.empty()) {
        outResult.message = "Failed to create temporary import directory.";
        return false;
    }
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; if (!path.empty()) fs::remove_all(path, ec); } } cleanup{staging};
    if (!runZipExtract(options.packagePath, staging, outResult.message)) return false;
    for (const ModuPakFileEntry& requested : options.entries) {
        if (!requested.selected || requested.archivePath.filename() == kManifestName) {
            outResult.skippedCount++;
            continue;
        }
        std::string reason;
        if (!isSafeArchivePath(requested.archivePath, &reason)) {
            outResult.message = "Rejected package: " + reason;
            return false;
        }
        fs::path source = staging / requested.archivePath;
        fs::path dest = options.projectRoot / requested.archivePath;
        std::error_code ec;
        bool replacing = fs::exists(dest, ec);
        if (!fs::exists(source, ec) || !fs::is_regular_file(source, ec)) continue;
        if (!copyFileChecked(source, dest, outResult.message)) return false;
        outResult.importedCount++;
        if (replacing) outResult.replacedCount++;
    }
    outResult.success = true;
    outResult.message = "Imported ModuPAK.";
    return true;
}

bool ModuObjExporter::exportObject(const fs::path& projectRoot,
                                   const fs::path& outputPath,
                                   const ModuPakManifest& manifest,
                                   const std::vector<SceneObject>& sceneObjects,
                                   const std::vector<int>& selectedObjectIds,
                                   int nextObjectId,
                                   ModuPakExportResult& outResult) {
    outResult = {};
    std::vector<SceneObject> hierarchy = collectObjectHierarchy(sceneObjects, selectedObjectIds);
    if (hierarchy.empty()) {
        outResult.message = "No scene objects selected.";
        return false;
    }
    fs::path staging = makeTempDirectory("moduobj_export");
    if (staging.empty()) {
        outResult.message = "Failed to create temporary export directory.";
        return false;
    }
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; if (!path.empty()) fs::remove_all(path, ec); } } cleanup{staging};
    if (!writeTextFile(staging / kManifestName, manifest.toText())) {
        outResult.message = "Failed to write ModuOBJ manifest.";
        return false;
    }
    if (!SceneSerializer::saveScene(staging / kObjectSceneName, hierarchy, nextObjectId, 0.5f, SkyboxSettings{})) {
        outResult.message = "Failed to serialize object hierarchy.";
        return false;
    }
    std::set<fs::path> deps;
    for (const SceneObject& obj : hierarchy) {
        collectDependency(deps, projectRoot, obj.meshPath);
        collectDependency(deps, projectRoot, obj.materialPath);
        collectDependency(deps, projectRoot, obj.albedoTexturePath);
        collectDependency(deps, projectRoot, obj.overlayTexturePath);
        collectDependency(deps, projectRoot, obj.normalMapPath);
        collectDependency(deps, projectRoot, obj.shaderPackPath);
        collectDependency(deps, projectRoot, obj.vertexShaderPath);
        collectDependency(deps, projectRoot, obj.fragmentShaderPath);
        for (const ScriptComponent& script : obj.scripts) collectDependency(deps, projectRoot, script.path);
    }
    for (const fs::path& dep : deps) {
        std::vector<ModuPakFileEntry> depEntries = ModuPakExporter::collectEntries(projectRoot, {dep}, false, outResult.message);
        fs::path archivePath = depEntries.empty() ? fs::relative(dep, projectRoot) : depEntries.front().archivePath;
        if (!copyFileChecked(dep, staging / "Dependencies" / archivePath, outResult.message)) return false;
        outResult.fileCount++;
    }
    outResult.fileCount += 2;
    if (!runZipCreate(staging, outputPath, outResult.message)) return false;
    std::error_code ec;
    outResult.packageSizeBytes = fs::file_size(outputPath, ec);
    outResult.success = true;
    outResult.message = "Exported ModuOBJ.";
    return true;
}

bool ModuObjImporter::importObject(const fs::path& projectRoot,
                                   const fs::path& packagePath,
                                   std::vector<SceneObject>& sceneObjects,
                                   int& nextObjectId,
                                   std::vector<int>* outImportedIds,
                                   ModuPakImportResult& outResult) {
    outResult = {};
    fs::path staging = makeTempDirectory("moduobj_import");
    if (staging.empty()) {
        outResult.message = "Failed to create temporary import directory.";
        return false;
    }
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; if (!path.empty()) fs::remove_all(path, ec); } } cleanup{staging};
    if (!runZipExtract(packagePath, staging, outResult.message)) return false;
    for (const fs::path& rel : listExtractedFiles(staging / "Dependencies")) {
        std::string reason;
        if (!ModuPakImporter::isSafeArchivePath(rel, &reason)) {
            outResult.message = "Rejected ModuOBJ dependency: " + reason;
            return false;
        }
        fs::path source = staging / "Dependencies" / rel;
        fs::path dest = projectRoot / rel;
        bool replacing = fs::exists(dest);
        if (!copyFileChecked(source, dest, outResult.message)) return false;
        outResult.importedCount++;
        if (replacing) outResult.replacedCount++;
    }
    std::vector<SceneObject> imported;
    int importedNextId = 0;
    int sceneVersion = 0;
    if (!SceneSerializer::loadScene(staging / kObjectSceneName, imported, importedNextId, sceneVersion, nullptr, nullptr, nullptr)) {
        outResult.message = "Failed to read ModuOBJ object hierarchy.";
        return false;
    }
    if (outImportedIds) outImportedIds->clear();
    remapImportedObjects(imported, nextObjectId, outImportedIds);
    for (SceneObject& obj : imported) sceneObjects.push_back(std::move(obj));
    outResult.importedCount += imported.size();
    outResult.success = true;
    outResult.message = "Imported ModuOBJ.";
    return true;
}
