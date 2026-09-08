#include "Engine.h"
#include "ModuPak.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace {
std::string formatBytes(uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << " " << units[unit];
    return out.str();
}

bool textFilled(const char* value) {
    return value && value[0] != '\0';
}

std::string safePackageFilename(const char* name) {
    std::string raw = textFilled(name) ? name : "NewPackage";
    std::string out;
    bool lastSep = false;
    for (unsigned char c : raw) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(c));
            lastSep = false;
        } else if (std::isspace(c) || c == '-' || c == '_') {
            if (!lastSep && !out.empty()) out.push_back('_');
            lastSep = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "NewPackage";
    return out + ".modupak";
}

void copyString(char* dest, size_t size, const std::string& value) {
    if (size == 0) return;
    std::snprintf(dest, size, "%s", value.c_str());
}

ModuPakManifest makeManifest(const char* name,
                             const char* author,
                             const char* description,
                             const char* version,
                             const char* packageType,
                             const fs::path& outputPath) {
    ModuPakManifest manifest;
    manifest.name = name ? name : "";
    manifest.author = author ? author : "";
    manifest.description = description ? description : "";
    manifest.version = textFilled(version) ? version : "1.0.0";
    manifest.packageType = packageType;
    manifest.subsystem = "ModuPAK";
    manifest.compatibleModuEngineVersion = "6.8+";
    manifest.packageID = ModuPakExporter::makePackageID(manifest.name, outputPath);
    return manifest;
}
}

void Engine::openModuPakExportDialog(const std::vector<fs::path>& seedPaths) {
    moduPakExportInputs = seedPaths;
    moduPakExportSelections.assign(moduPakExportInputs.size(), true);
    if (moduPakVersion[0] == '\0') copyString(moduPakVersion, sizeof(moduPakVersion), "1.0.0");
    if (projectManager.currentProject.isLoaded) {
        const fs::path currentOut(moduPakOutputPath);
        const bool shouldUseProjectDefault =
            moduPakOutputPath[0] == '\0' ||
            currentOut.filename() == "NewPackage.modupak" ||
            currentOut.parent_path().filename() == "Exports";
        if (shouldUseProjectDefault) {
            fs::path out = projectManager.currentProject.assetsPath / safePackageFilename(moduPakPackageName);
            copyString(moduPakOutputPath, sizeof(moduPakOutputPath), out.string());
        }
    }
    showModuPakExportDialog = true;
}

void Engine::openModuPakImportDialog(const fs::path& packagePath) {
    if (!packagePath.empty()) copyString(moduPakImportPath, sizeof(moduPakImportPath), packagePath.string());
    moduPakImportFeedback.clear();
    moduPakImportEntries.clear();
    moduPakImportManifest = {};
    if (textFilled(moduPakImportPath) && projectManager.currentProject.isLoaded) {
        ModuPakImportPreview preview;
        if (ModuPakImporter::readPreview(moduPakImportPath, projectManager.currentProject.projectPath, preview)) {
            moduPakImportEntries = preview.entries;
            moduPakImportManifest = preview.manifest;
        } else {
            moduPakImportFeedback = preview.message;
        }
    }
    showModuPakImportDialog = true;
}

void Engine::openModuObjExportDialog() {
    if (moduObjVersion[0] == '\0') copyString(moduObjVersion, sizeof(moduObjVersion), "1.0.0");
    if (moduObjName[0] == '\0') {
        if (SceneObject* selected = getSelectedObject()) copyString(moduObjName, sizeof(moduObjName), selected->name);
        else copyString(moduObjName, sizeof(moduObjName), "SceneObject");
    }
    if (moduObjOutputPath[0] == '\0' && projectManager.currentProject.isLoaded) {
        fs::path out = projectManager.currentProject.projectPath / "Assets" / (std::string(moduObjName) + ".moduobj");
        copyString(moduObjOutputPath, sizeof(moduObjOutputPath), out.string());
    }
    showModuObjExportDialog = true;
}

void Engine::openModuObjImportDialog(const fs::path& packagePath) {
    if (!packagePath.empty()) copyString(moduObjImportPath, sizeof(moduObjImportPath), packagePath.string());
    showModuObjImportDialog = true;
}

void Engine::renderModuPakExportDialog() {
    if (!showModuPakExportDialog) return;
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    ImGui::OpenPopup("Export Into ModuPAK");
    if (ImGui::BeginPopupModal("Export Into ModuPAK", &showModuPakExportDialog, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::InputText("Package Name", moduPakPackageName, sizeof(moduPakPackageName));
        ImGui::InputText("Author Name", moduPakAuthorName, sizeof(moduPakAuthorName));
        ImGui::InputTextMultiline("Description", moduPakDescription, sizeof(moduPakDescription), ImVec2(-1, 64));
        ImGui::InputText("Version", moduPakVersion, sizeof(moduPakVersion));
        ImGui::InputText("Output", moduPakOutputPath, sizeof(moduPakOutputPath));
        ImGui::InputText("Add Path", moduPakAddPath, sizeof(moduPakAddPath));
        ImGui::SameLine();
        if (ImGui::Button("Add Files") && textFilled(moduPakAddPath)) {
            moduPakExportInputs.push_back(moduPakAddPath);
            moduPakExportSelections.push_back(true);
            moduPakAddPath[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Folder") && textFilled(moduPakAddPath)) {
            moduPakExportInputs.push_back(moduPakAddPath);
            moduPakExportSelections.push_back(true);
            moduPakAddPath[0] = '\0';
        }
        ImGui::Checkbox("Recursive folders", &moduPakRecursiveFolders);
        ImGui::InputTextWithHint("Search", "filter assets", moduPakSearch, sizeof(moduPakSearch));

        if (ImGui::Button("Remove Selected")) {
            for (int i = static_cast<int>(moduPakExportInputs.size()) - 1; i >= 0; --i) {
                if (i < static_cast<int>(moduPakExportSelections.size()) && !moduPakExportSelections[i]) {
                    moduPakExportInputs.erase(moduPakExportInputs.begin() + i);
                    moduPakExportSelections.erase(moduPakExportSelections.begin() + i);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            moduPakExportInputs.clear();
            moduPakExportSelections.clear();
        }

        ImGui::BeginChild("ModuPakExportAssets", ImVec2(0, 230), true);
        std::string filter = moduPakSearch;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (size_t i = 0; i < moduPakExportInputs.size(); ++i) {
            std::string label = moduPakExportInputs[i].string();
            std::string lower = label;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
            if (i >= moduPakExportSelections.size()) moduPakExportSelections.push_back(true);
            bool selected = moduPakExportSelections[i] != 0;
            if (ImGui::Checkbox(("##asset" + std::to_string(i)).c_str(), &selected)) {
                moduPakExportSelections[i] = selected ? 1 : 0;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
        }
        ImGui::EndChild();

        const bool canExport = textFilled(moduPakPackageName) && textFilled(moduPakAuthorName) && textFilled(moduPakOutputPath);
        if (!canExport) ImGui::BeginDisabled();
        if (ImGui::Button("Export", ImVec2(120, 0))) {
            std::vector<fs::path> selected;
            for (size_t i = 0; i < moduPakExportInputs.size(); ++i) {
                if (i < moduPakExportSelections.size() && moduPakExportSelections[i]) selected.push_back(moduPakExportInputs[i]);
            }
            ModuPakExportOptions options;
            options.projectRoot = projectManager.currentProject.projectPath;
            options.outputPath = moduPakOutputPath;
            options.inputPaths = std::move(selected);
            options.recursiveFolders = moduPakRecursiveFolders;
            options.manifest = makeManifest(moduPakPackageName, moduPakAuthorName, moduPakDescription, moduPakVersion, "Standalone Package", options.outputPath);
            ModuPakExportResult result;
            if (ModuPakExporter::exportPackage(options, result)) {
                addConsoleMessage("Exported ModuPAK '" + options.manifest.name + "' (" +
                                  std::to_string(result.fileCount) + " files, " +
                                  formatBytes(result.packageSizeBytes) + "): " +
                                  options.outputPath.string(), ConsoleMessageType::Success);
                showEditorToast("Exported ModuPAK: " + options.manifest.name, ConsoleMessageType::Success, 2.0);
                showModuPakExportDialog = false;
                ImGui::CloseCurrentPopup();
            } else {
                addConsoleMessage("ModuPAK export failed: " + result.message, ConsoleMessageType::Error);
            }
        }
        if (!canExport) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            showModuPakExportDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Engine::renderModuPakImportDialog() {
    if (!showModuPakImportDialog) return;
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    ImGui::OpenPopup("Import ModuPAK");
    if (ImGui::BeginPopupModal("Import ModuPAK", &showModuPakImportDialog, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::InputText("Package", moduPakImportPath, sizeof(moduPakImportPath));
        ImGui::SameLine();
        if (ImGui::Button("Load")) openModuPakImportDialog(moduPakImportPath);
        ImGui::Text("Name: %s", moduPakImportManifest.name.c_str());
        ImGui::Text("Author: %s   Version: %s", moduPakImportManifest.author.c_str(), moduPakImportManifest.version.c_str());
        if (!moduPakImportManifest.description.empty()) ImGui::TextWrapped("%s", moduPakImportManifest.description.c_str());
        if (!moduPakImportManifest.compatibleModuEngineVersion.empty() &&
            moduPakImportManifest.compatibleModuEngineVersion != "6.8+") {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "Warning: package targets %s", moduPakImportManifest.compatibleModuEngineVersion.c_str());
        }
        if (!moduPakImportFeedback.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", moduPakImportFeedback.c_str());
        }
        if (ImGui::Button("All")) {
            for (auto& entry : moduPakImportEntries) if (entry.archivePath.filename() != "Manifest.modu") entry.selected = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("None")) {
            for (auto& entry : moduPakImportEntries) entry.selected = false;
        }
        ImGui::BeginChild("ModuPakImportTree", ImVec2(0, 300), true);
        for (size_t i = 0; i < moduPakImportEntries.size(); ++i) {
            ModuPakFileEntry& entry = moduPakImportEntries[i];
            ImGui::Checkbox(("##import" + std::to_string(i)).c_str(), &entry.selected);
            ImGui::SameLine();
            ImVec4 tagColor = entry.statusTag == "REPLACE" ? ImVec4(0.9f, 0.65f, 0.2f, 1.0f)
                              : entry.statusTag == "SKIP" ? ImVec4(0.55f, 0.6f, 0.65f, 1.0f)
                              : ImVec4(0.25f, 0.85f, 0.45f, 1.0f);
            ImGui::TextColored(tagColor, "%s", entry.statusTag.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(entry.archivePath.generic_string().c_str());
        }
        ImGui::EndChild();
        if (ImGui::Button("Import", ImVec2(120, 0))) {
            ModuPakImportOptions options;
            options.projectRoot = projectManager.currentProject.projectPath;
            options.packagePath = moduPakImportPath;
            options.entries = moduPakImportEntries;
            ModuPakImportResult result;
            if (ModuPakImporter::importPackage(options, result)) {
                fileBrowser.needsRefresh = true;
                addConsoleMessage("Imported ModuPAK '" + moduPakImportManifest.name + "': " +
                                  std::to_string(result.importedCount) + " files, " +
                                  std::to_string(result.replacedCount) + " replaced.",
                                  ConsoleMessageType::Success);
                showModuPakImportDialog = false;
                ImGui::CloseCurrentPopup();
            } else {
                addConsoleMessage("ModuPAK import failed: " + result.message, ConsoleMessageType::Error);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            showModuPakImportDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Engine::renderModuObjExportDialog() {
    if (!showModuObjExportDialog) return;
    ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
    ImGui::OpenPopup("Export As ModuOBJ");
    if (ImGui::BeginPopupModal("Export As ModuOBJ", &showModuObjExportDialog, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::InputText("Object Name", moduObjName, sizeof(moduObjName));
        ImGui::InputText("Author Name", moduObjAuthor, sizeof(moduObjAuthor));
        ImGui::InputTextMultiline("Description", moduObjDescription, sizeof(moduObjDescription), ImVec2(-1, 64));
        ImGui::InputText("Version", moduObjVersion, sizeof(moduObjVersion));
        ImGui::InputText("Output", moduObjOutputPath, sizeof(moduObjOutputPath));
        const bool canExport = textFilled(moduObjName) && textFilled(moduObjAuthor) && textFilled(moduObjOutputPath) &&
                               (!selectedObjectIds.empty() || selectedObjectId >= 0);
        if (!canExport) ImGui::BeginDisabled();
        if (ImGui::Button("Export", ImVec2(120, 0))) {
            std::vector<int> ids = selectedObjectIds;
            if (ids.empty() && selectedObjectId >= 0) ids.push_back(selectedObjectId);
            ModuPakManifest manifest = makeManifest(moduObjName, moduObjAuthor, moduObjDescription, moduObjVersion, "ModuOBJ", moduObjOutputPath);
            ModuPakExportResult result;
            if (ModuObjExporter::exportObject(projectManager.currentProject.projectPath, moduObjOutputPath, manifest, sceneObjects, ids, nextObjectId, result)) {
                addConsoleMessage("Exported ModuOBJ '" + manifest.name + "' (" + std::to_string(result.fileCount) +
                                  " files, " + formatBytes(result.packageSizeBytes) + "): " +
                                  std::string(moduObjOutputPath), ConsoleMessageType::Success);
                showModuObjExportDialog = false;
                ImGui::CloseCurrentPopup();
            } else {
                addConsoleMessage("ModuOBJ export failed: " + result.message, ConsoleMessageType::Error);
            }
        }
        if (!canExport) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            showModuObjExportDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Engine::renderModuObjImportDialog() {
    if (!showModuObjImportDialog) return;
    ImGui::SetNextWindowSize(ImVec2(560, 260), ImGuiCond_FirstUseEver);
    ImGui::OpenPopup("Import ModuOBJ");
    if (ImGui::BeginPopupModal("Import ModuOBJ", &showModuObjImportDialog, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::InputText("ModuOBJ", moduObjImportPath, sizeof(moduObjImportPath));
        if (ImGui::Button("Import", ImVec2(120, 0))) {
            ModuPakImportResult result;
            std::vector<int> importedIds;
            if (ModuObjImporter::importObject(projectManager.currentProject.projectPath, moduObjImportPath, sceneObjects, nextObjectId, &importedIds, result)) {
                selectedObjectIds = importedIds;
                selectedObjectId = selectedObjectIds.empty() ? -1 : selectedObjectIds.back();
                updateHierarchyWorldTransforms();
                markRuntimeScriptBindingsDirty();
                projectManager.currentProject.hasUnsavedChanges = true;
                fileBrowser.needsRefresh = true;
                addConsoleMessage("Imported ModuOBJ: " + std::to_string(importedIds.size()) + " object(s), " +
                                  std::to_string(result.replacedCount) + " dependencies replaced.",
                                  ConsoleMessageType::Success);
                showModuObjImportDialog = false;
                ImGui::CloseCurrentPopup();
            } else {
                addConsoleMessage("ModuOBJ import failed: " + result.message, ConsoleMessageType::Error);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            showModuObjImportDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
