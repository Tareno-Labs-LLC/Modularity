#include "EditorLocalization.h"
namespace Loc = Modularity::Loc;
#if defined(_WIN32) && !defined(__ANDROID__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "Engine.h"
#include "AndroidToolchain.h"
#include "DevBuildDir.h"
#include "ScriptContextBinding.h"
#include "Modu2DStats.h"
#ifdef __ANDROID__
#include "AndroidRuntime/AndroidRuntime.h"
#include <android/keycodes.h>
#endif
#include "SceneSerializationInternal.h"
#include "DurableFile.h"
#include "GLSyncProbe.h"
#include "AudioExportCompression.h"
#include "AnimationBindingHelpers.h"
#include "CrashReporter.h"
#include "DragPreviewOverlay.h"
#include "UiGlassBlur.h"
#include "HardwareProfile.h"
#include "MapMaker.h"
#include "MaterialAssetUtils.h"
#include "EngineMaterialIO.h"
#include "ModelLoader.h"
#include "../include/Platform/AssetSource.h"
#include "Render25D/MMeshConvert.h"
#include "Render25D/MMeshLoader.h"
#include "RuntimeContent.h"
#include "SceneAssetReferences.h"
#include <iostream>
#include <map>
#include <fstream>
#include <functional>
#include <chrono>
#include <thread>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <unordered_set>
#include <unordered_map>
#include <cmath>
#include <cctype>
#include <cstring>
#include <ctime>
#include <regex>
#include "../include/Textures/TextureTranscode.h"
#include "../include/Textures/StbFlipScope.h"
#include "ParticleSystemRuntime.h"
#include "DiscordPresence.h"
#include "../include/Shaders/Gles2ShaderTranspiler.h"
#include <set>
#include <sstream>
#if defined(_WIN32)
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif
#include "ThirdParty/glm/gtc/constants.hpp"
#include "ThirdParty/glfw/deps/stb_image_write.h"

#pragma region Material File IO Helpers
namespace {
constexpr int kRuntimeInternalWidth = 1280;
constexpr int kRuntimeInternalHeight = 720;
constexpr char kRuntimeCacheReadyMarker[] = ".runtime_bundle_ready";
constexpr size_t kRuntimeCacheMaxEntries = 3;
constexpr auto kRuntimeCacheMaxAge = std::chrono::hours(24 * 7);
constexpr char kDefaultUIFontAssetId[] = "builtin:imgui-default";
constexpr float kUIFontAtlasBaseSize = 18.0f;

bool IsNativeBinaryPath(const fs::path& path) {
    const std::string ext = path.extension().string();
    return ext == ".so" || ext == ".dll" || ext == ".dylib";
}

int AllocateNextRig25DNodeId(const std::vector<SceneObject>& objects) {
    int nextNodeId = 0;
    for (const SceneObject& obj : objects) {
        if (!obj.hasRig25DNode || obj.rig25DNode.nodeId < 0) {
            continue;
        }
        nextNodeId = std::max(nextNodeId, obj.rig25DNode.nodeId + 1);
    }
    return nextNodeId;
}

// Console lines for a scene file that exists but did not load. When a default scene takes its
// place, that scene is bound to the same path and the next save would overwrite a file that may
// only need a newer engine or a hand fix, so its bytes are copied aside first.
std::vector<std::pair<std::string, ConsoleMessageType>> DescribeUnreadableScene(
    const std::string& sceneName, const fs::path& scenePath, bool defaultSceneTakesItsPlace) {
    std::vector<std::pair<std::string, ConsoleMessageType>> lines;
    if (Modularity::FileContainsNulBytes(scenePath)) {
        lines.emplace_back("Error: Scene '" + sceneName + "' " + Modularity::kDamagedSceneFileMessage,
                           ConsoleMessageType::Error);
        return lines;
    }
    lines.emplace_back("Error: Failed to load scene: " + sceneName, ConsoleMessageType::Error);
    if (!defaultSceneTakesItsPlace) return lines;

    std::error_code ec;
    if (!fs::is_regular_file(scenePath, ec) || fs::file_size(scenePath, ec) == 0 || ec) return lines;
    fs::path keep = scenePath;
    keep += ".unreadable";
    fs::copy_file(scenePath, keep, fs::copy_options::skip_existing, ec);
    if (!ec && fs::exists(keep, ec)) {
        lines.emplace_back("A copy of the unreadable scene was kept at: " + keep.string(),
                           ConsoleMessageType::Warning);
    }
    return lines;
}

ConsoleMessageType DiagnosticConsoleType(Modularity::ScriptDiagnosticSeverity severity) {
    switch (severity) {
        case Modularity::ScriptDiagnosticSeverity::Error:
            return ConsoleMessageType::Error;
        case Modularity::ScriptDiagnosticSeverity::Warning:
            return ConsoleMessageType::Warning;
        case Modularity::ScriptDiagnosticSeverity::Info:
        default:
            return ConsoleMessageType::Info;
    }
}

std::string RenameTrim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

bool IsFontFileExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext == ".ttf" || ext == ".otf";
}

std::string EncodeStyleBlob(const ImGuiStyle& style) {
    static const char kHex[] = "0123456789ABCDEF";
    const auto* bytes = reinterpret_cast<const unsigned char*>(&style);
    std::string encoded;
    encoded.resize(sizeof(ImGuiStyle) * 2);
    for (size_t i = 0; i < sizeof(ImGuiStyle); ++i) {
        encoded[i * 2] = kHex[(bytes[i] >> 4) & 0x0F];
        encoded[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    return encoded;
}

bool DecodeStyleBlob(const std::string& encoded, ImGuiStyle& outStyle) {
    if (encoded.empty() || (encoded.size() % 2) != 0 || encoded.size() > sizeof(ImGuiStyle) * 2) {
        return false;
    }
    auto decodeNibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };

    const size_t byteCount = encoded.size() / 2;
    ImGuiStyle decoded = outStyle;
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&decoded);
    for (size_t i = 0; i < byteCount; ++i) {
        const int hi = decodeNibble(encoded[i * 2]);
        const int lo = decodeNibble(encoded[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    outStyle = decoded;
    return true;
}

#pragma region Shade Theme Serialization
const int kShadeFormatVersion = 1;

void WriteShadeTheme(std::ostream& out, const std::string& prefix, const ImGuiShadeTheme& theme) {
    out << prefix << "shade=" << (theme.Enabled ? 1 : 0)
        << " " << theme.GradientScale << " " << theme.BevelScale
        << " " << kShadeFormatVersion << "\n";
    if (!theme.Enabled) {
        return;
    }
    for (int c = 0; c < ImGuiShadeClass_COUNT; ++c) {
        for (int s = 0; s < ImGuiShadeState_COUNT; ++s) {
            const ImGuiShadeParams& p = theme.Params[c][s];
            if ((p.Flags & ImGuiShadeFlags_Set) == 0) {
                continue;
            }
            std::ostringstream entry;
            entry << p.Flags << " " << p.GradientTop << " " << p.GradientBottom << " "
                  << p.BevelSize << " " << p.BevelIntensity << " " << p.BorderSize << " "
                  << std::hex << std::showbase
                  << p.ColTopHighlight << " " << p.ColBottomShadow << " "
                  << p.ColInnerShadow << " " << p.ColInnerHighlight << " "
                  << p.ColBorderTop << " " << p.ColBorderBottom << " "
                  << p.ColBorderLeft << " " << p.ColBorderRight << " " << p.ColBorderAll;
            out << prefix << "shade." << ImGui::GetShadeClassName(c) << "."
                << ImGui::GetShadeStateName(s) << "=" << entry.str() << "\n";
        }
    }
}

bool ShadeParamsDrawsSomething(const ImGuiShadeParams& p) {
    if ((p.Flags & ImGuiShadeFlags_Set) == 0) return false;
    if (p.GradientTop != 0.0f || p.GradientBottom != 0.0f) return true;
    if (p.BevelSize > 0.0f || p.BorderSize > 0.0f) return true;
    return (p.ColTopHighlight | p.ColBottomShadow | p.ColInnerShadow | p.ColInnerHighlight
            | p.ColBorderTop | p.ColBorderBottom | p.ColBorderLeft | p.ColBorderRight
            | p.ColBorderAll) != 0;
}

bool ShadeThemeHasVisibleShading(const ImGuiShadeTheme& theme) {
    for (int c = 0; c < ImGuiShadeClass_COUNT; ++c) {
        for (int s = 0; s < ImGuiShadeState_COUNT; ++s) {
            if (ShadeParamsDrawsSomething(theme.Params[c][s])) return true;
        }
    }
    return false;
}

bool ParseShadeThemeField(const std::string& field, const std::string& value, ImGuiShadeTheme& theme,
                          int* outFormatVersion) {
    if (field.rfind("shade", 0) != 0) {
        return false;
    }
    if (field == "shade") {
        std::istringstream ss(value);
        int enabled = 0;
        float gradientScale = 1.0f;
        float bevelScale = 1.0f;
        int formatVersion = 0;
        if (ss >> enabled) {
            theme.Enabled = (enabled != 0);
            if (ss >> gradientScale) theme.GradientScale = gradientScale;
            if (ss >> bevelScale) theme.BevelScale = bevelScale;
            if (ss >> formatVersion && outFormatVersion) *outFormatVersion = formatVersion;
        }
        return true;
    }
    if (field.size() < 7 || field[5] != '.') {
        return true;
    }
    const std::string path = field.substr(6);
    const size_t dot = path.find('.');
    if (dot == std::string::npos) {
        return true;
    }
    const ImGuiShadeClass shadeClass = ImGui::FindShadeClassByName(path.substr(0, dot).c_str());
    const ImGuiShadeState shadeState = ImGui::FindShadeStateByName(path.substr(dot + 1).c_str());
    if (shadeClass < 0 || shadeState < 0) {
        return true;
    }

    ImGuiShadeParams p;
    std::istringstream ss(value);
    std::string flagsToken;
    if (!(ss >> flagsToken)) {
        return true;
    }
    try {
        p.Flags = static_cast<ImGuiShadeFlags>(std::stoul(flagsToken, nullptr, 0));
    } catch (...) {
        return true;
    }
    ss >> p.GradientTop >> p.GradientBottom >> p.BevelSize >> p.BevelIntensity >> p.BorderSize;
    ImU32* colors[] = {
        &p.ColTopHighlight, &p.ColBottomShadow, &p.ColInnerShadow, &p.ColInnerHighlight,
        &p.ColBorderTop, &p.ColBorderBottom, &p.ColBorderLeft, &p.ColBorderRight, &p.ColBorderAll,
    };
    for (ImU32* color : colors) {
        std::string token;
        if (!(ss >> token)) break;
        try {
            *color = static_cast<ImU32>(std::stoul(token, nullptr, 0));
        } catch (...) {
            *color = 0;
        }
    }
    p.Flags |= ImGuiShadeFlags_Set;
    theme.Params[shadeClass][shadeState] = p;
    return true;
}
#pragma endregion

bool RenameIsInteger(const std::string& value) {
    std::string trimmed = RenameTrim(value);
    if (trimmed.empty()) return false;
    size_t start = (trimmed[0] == '-' || trimmed[0] == '+') ? 1 : 0;
    if (start >= trimmed.size()) return false;
    for (size_t i = start; i < trimmed.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(trimmed[i]))) {
            return false;
        }
    }
    return true;
}

void RenameReplaceTrimmed(std::string& target, const std::string& replacement) {
    size_t start = 0;
    while (start < target.size() &&
           std::isspace(static_cast<unsigned char>(target[start])) != 0) {
        ++start;
    }
    size_t end = target.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(target[end - 1])) != 0) {
        --end;
    }
    target = target.substr(0, start) + replacement + target.substr(end);
}

std::vector<std::string> RenameSplitEscaped(const std::string& value, char delimiter) {
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (char c : value) {
        if (escaped) {
            if (c == 'n') current.push_back('\n');
            else if (c == 'r') current.push_back('\r');
            else if (c == 't') current.push_back('\t');
            else current.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == delimiter) {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (escaped) current.push_back('\\');
    fields.push_back(current);
    return fields;
}

std::string RenameEscapeField(const std::string& value, char delimiter) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '\\' || c == delimiter || c == '\n' || c == '\r' || c == '\t') {
            out.push_back('\\');
            if (c == '\n') out.push_back('n');
            else if (c == '\r') out.push_back('r');
            else if (c == '\t') out.push_back('t');
            else out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string RenameJoinEscaped(const std::vector<std::string>& values, char delimiter) {
    std::string joined;
    for (size_t i = 0; i < values.size(); ++i) {
        joined += RenameEscapeField(values[i], delimiter);
        if (i + 1 < values.size()) joined.push_back(delimiter);
    }
    return joined;
}

bool RenameRewriteSingleObjectRef(std::string& value,
                                  const std::string& oldName,
                                  const std::string& newName) {
    std::string trimmed = RenameTrim(value);
    if (trimmed.empty()) return false;

    if (trimmed == oldName) {
        RenameReplaceTrimmed(value, newName);
        return true;
    }

    const std::string namePrefix = "Object.";
    if (trimmed.rfind(namePrefix, 0) == 0) {
        const std::string suffix = trimmed.substr(namePrefix.size());
        if (suffix == oldName) {
            RenameReplaceTrimmed(value, namePrefix + newName);
            return true;
        }
    }

    return false;
}

bool RenameRewriteManagedObjectSetting(std::string& value,
                                       const std::string& oldName,
                                       const std::string& newName) {
    const size_t firstBar = value.find('|');
    if (firstBar == std::string::npos) return false;
    if (value.find('|', firstBar + 1) != std::string::npos) return false;

    std::string idPart = value.substr(0, firstBar);
    if (!RenameIsInteger(idPart)) return false;

    std::string namePart = value.substr(firstBar + 1);
    std::string trimmedName = RenameTrim(namePart);
    if (trimmedName != oldName) return false;

    RenameReplaceTrimmed(namePart, newName);
    value = idPart + "|" + namePart;
    return true;
}

bool RenameRewriteObjectRefList(std::string& value,
                                const std::string& oldName,
                                const std::string& newName) {
    if (value.find(';') == std::string::npos) return false;

    std::vector<std::string> refs = RenameSplitEscaped(value, ';');
    bool changed = false;
    for (std::string& ref : refs) {
        changed |= RenameRewriteSingleObjectRef(ref, oldName, newName);
    }
    if (!changed) return false;

    value = RenameJoinEscaped(refs, ';');
    return true;
}

bool RenameRewriteDialogueLines(std::string& value,
                                const std::string& oldName,
                                const std::string& newName) {
    if (value.find('|') == std::string::npos) return false;

    char outerDelimiter = '\t';
    if (value.find('\t') == std::string::npos && value.find('\n') != std::string::npos) {
        outerDelimiter = '\n';
    }

    std::vector<std::string> lines = RenameSplitEscaped(value, outerDelimiter);
    bool changed = false;
    for (std::string& line : lines) {
        if (RenameTrim(line).empty()) continue;

        std::vector<std::string> fields = RenameSplitEscaped(line, '|');
        if (fields.size() < 14) continue;

        bool lineChanged = false;
        lineChanged |= RenameRewriteSingleObjectRef(fields[9], oldName, newName);
        lineChanged |= RenameRewriteSingleObjectRef(fields[10], oldName, newName);
        lineChanged |= RenameRewriteSingleObjectRef(fields[11], oldName, newName);
        lineChanged |= RenameRewriteObjectRefList(fields[12], oldName, newName);
        lineChanged |= RenameRewriteObjectRefList(fields[13], oldName, newName);
        if (lineChanged) {
            line = RenameJoinEscaped(fields, '|');
            changed = true;
        }
    }

    if (!changed) return false;
    value = RenameJoinEscaped(lines, outerDelimiter);
    return true;
}

bool RenameRewriteInteractableOptions(std::string& value,
                                      const std::string& oldName,
                                      const std::string& newName) {
    if (value.find('|') == std::string::npos) return false;

    char outerDelimiter = '\t';
    if (value.find('\t') == std::string::npos && value.find('\n') != std::string::npos) {
        outerDelimiter = '\n';
    }

    std::vector<std::string> options = RenameSplitEscaped(value, outerDelimiter);
    bool changed = false;
    for (std::string& option : options) {
        if (RenameTrim(option).empty()) continue;

        std::vector<std::string> fields = RenameSplitEscaped(option, '|');
        if (fields.size() < 8) continue;

        bool optionChanged = false;
        optionChanged |= RenameRewriteSingleObjectRef(fields[2], oldName, newName);
        optionChanged |= RenameRewriteDialogueLines(fields[3], oldName, newName);
        optionChanged |= RenameRewriteObjectRefList(fields[4], oldName, newName);
        optionChanged |= RenameRewriteObjectRefList(fields[5], oldName, newName);
        optionChanged |= RenameRewriteObjectRefList(fields[6], oldName, newName);
        optionChanged |= RenameRewriteObjectRefList(fields[7], oldName, newName);
        if (optionChanged) {
            option = RenameJoinEscaped(fields, '|');
            changed = true;
        }
    }

    if (!changed) return false;
    value = RenameJoinEscaped(options, outerDelimiter);
    return true;
}

bool RenameRewriteScriptSettingValue(std::string& value,
                                     const std::string& oldName,
                                     const std::string& newName) {
    if (value.empty()) return false;

    bool changed = false;
    changed |= RenameRewriteManagedObjectSetting(value, oldName, newName);
    changed |= RenameRewriteSingleObjectRef(value, oldName, newName);
    changed |= RenameRewriteObjectRefList(value, oldName, newName);
    changed |= RenameRewriteDialogueLines(value, oldName, newName);
    changed |= RenameRewriteInteractableOptions(value, oldName, newName);
    return changed;
}

bool IsDefaultTransform(const SceneObject& obj) {
    auto nearZero = [](float v) { return std::abs(v) < 1e-4f; };
    auto nearOne = [](float v) { return std::abs(v - 1.0f) < 1e-4f; };
    return nearZero(obj.localPosition.x) &&
           nearZero(obj.localPosition.y) &&
           nearZero(obj.localPosition.z) &&
           nearZero(obj.localRotation.x) &&
           nearZero(obj.localRotation.y) &&
           nearZero(obj.localRotation.z) &&
           nearOne(obj.localScale.x) &&
           nearOne(obj.localScale.y) &&
           nearOne(obj.localScale.z);
}

void ApplyModelRootTransform(SceneObject& obj, const ModelSceneData& sceneData) {
    if (sceneData.nodes.empty()) return;
    if (obj.localInitialized && !IsDefaultTransform(obj)) return;
    const auto& root = sceneData.nodes.front();
    obj.localPosition = root.localPosition;
    obj.localRotation = root.localRotation;
    obj.localScale = root.localScale;
    obj.localInitialized = true;
    obj.position = obj.localPosition;
    obj.rotation = obj.localRotation;
    obj.scale = obj.localScale;
}

std::string sanitizeMaterialName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-') {
            out.push_back(c);
        } else if (c == ' ' || c == '.') {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "Material";
    return out;
}

class ModelTextureResolver {
public:
    ModelTextureResolver(fs::path searchRoot, fs::path projectRoot)
        : root(std::move(searchRoot)), project(std::move(projectRoot)) {}

    enum class Role { Albedo, Normal, Roughness, Metallic, Opacity, Emissive, Occlusion };

    int resolvedByName() const { return foundByName; }
    int unresolved() const { return missing; }
    int discoveredByConvention() const { return discovered; }

    std::string discover(Role role, const std::string& materialName, const fs::path& modelPath) {
        std::vector<std::string> bases;
        auto addBase = [&bases](std::string value) {
            value = lowerAscii(std::move(value));
            if (value.empty()) return;
            for (const char* suffix : {"_1k", "_2k", "_4k", "_8k", "_16k"}) {
                const size_t n = std::strlen(suffix);
                if (value.size() > n && value.compare(value.size() - n, n, suffix) == 0) {
                    value.erase(value.size() - n);
                    break;
                }
            }
            if (!value.empty() &&
                std::find(bases.begin(), bases.end(), value) == bases.end()) {
                bases.push_back(value);
            }
        };
        addBase(materialName);
        addBase(modelPath.stem().string());
        addBase(modelPath.parent_path().filename().string());
        if (bases.empty()) return "";

        const fs::path modelDir = modelPath.parent_path();
        std::error_code ec;
        for (const char* sub : {"textures", "Textures"}) {
            if (std::string hit = scanDirectory(modelDir / sub, role, bases); !hit.empty()) {
                return hit;
            }
        }
        if (std::string hit = scanDirectory(modelDir, role, bases); !hit.empty()) {
            return hit;
        }
        ensureIndex();
        return pickBest(allTextures, role, bases);
    }

    std::string resolve(const std::string& texPath, const fs::path& modelPath,
                        Role role, const std::string& materialName) {
        if (!texPath.empty() && texPath[0] == '*') return "";
        if (texPath.empty()) return discover(role, materialName, modelPath);

        std::string cleaned = texPath;
        std::replace(cleaned.begin(), cleaned.end(), '\\', '/');
        while (cleaned.rfind("//", 0) == 0) {
            cleaned.erase(0, 2);
        }
        while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '\t')) {
            cleaned.erase(cleaned.begin());
        }
        if (cleaned.empty()) return "";

        const fs::path raw(cleaned);
        const fs::path modelDir = modelPath.parent_path();
        const std::string filename = raw.filename().string();

        std::vector<fs::path> candidates;
        candidates.reserve(8);
        if (raw.is_absolute()) {
            candidates.push_back(raw);
        }
        candidates.push_back(modelDir / raw);
        if (!filename.empty()) {
            candidates.push_back(modelDir / filename);
            for (const char* sub : {"textures", "Textures", "tex", "maps", "images"}) {
                candidates.push_back(modelDir / sub / filename);
            }
        }

        std::error_code ec;
        for (const fs::path& candidate : candidates) {
            if (fs::is_regular_file(candidate, ec)) {
                return present(candidate);
            }
        }

        ensureIndex();
        if (const fs::path* hit = lookup(byFilename, filename)) {
            ++foundByName;
            return present(*hit);
        }
        if (const fs::path* hit = lookup(byStem, raw.stem().string())) {
            ++foundByName;
            return present(*hit);
        }

        ++missing;
        return (raw.is_absolute() ? raw : (modelDir / raw)).string();
    }

private:
    static bool matchesRole(const std::string& stem, Role role) {
        struct RoleTokens {
            std::vector<std::string> want;
            std::vector<std::string> reject;
        };
        static const RoleTokens kAlbedo = {
            {"diff", "albedo", "basecolor", "base_color", "color", "_col"},
            {"rough", "metal", "spec", "_ao", "arm", "disp", "height", "bump",
             "emissive", "emission", "mask", "opacity", "alpha", "nor", "gloss"}};
        static const RoleTokens kNormal = {
            {"nor", "normal", "nrm"},
            {"rough", "metal", "spec", "_ao", "arm", "disp", "height",
             "emissive", "emission", "mask", "opacity", "alpha", "diff", "gloss"}};
        static const RoleTokens kRoughness = {
            {"rough", "_rgh"},
            {"metal", "arm", "gloss", "diff", "albedo", "color", "nor",
             "emissive", "emission", "opacity", "alpha", "_ao", "disp", "height"}};
        static const RoleTokens kMetallic = {
            {"metal", "metalness", "metallic", "_mtl"},
            {"rough", "arm", "gloss", "diff", "albedo", "color", "nor",
             "emissive", "emission", "opacity", "alpha", "_ao", "disp", "height"}};
        static const RoleTokens kOpacity = {
            {"opacity", "alpha", "_mask", "transparen"},
            {"rough", "metal", "arm", "gloss", "diff", "albedo", "color", "nor",
             "emissive", "emission", "_ao", "disp", "height"}};
        static const RoleTokens kEmissive = {
            {"emissive", "emission", "_emit"},
            {"rough", "metal", "arm", "gloss", "diff", "albedo", "color", "nor",
             "opacity", "alpha", "_ao", "disp", "height"}};
        static const RoleTokens kOcclusion = {
            {"_ao", "ambientocclusion", "ambient_occlusion", "occlusion"},
            {"rough", "metal", "arm", "gloss", "diff", "albedo", "color", "nor",
             "emissive", "emission", "opacity", "alpha", "disp", "height"}};

        const RoleTokens* tokens = &kAlbedo;
        switch (role) {
            case Role::Normal:    tokens = &kNormal;    break;
            case Role::Roughness: tokens = &kRoughness; break;
            case Role::Metallic:  tokens = &kMetallic;  break;
            case Role::Opacity:   tokens = &kOpacity;   break;
            case Role::Emissive:  tokens = &kEmissive;  break;
            case Role::Occlusion: tokens = &kOcclusion; break;
            case Role::Albedo:
            default:              tokens = &kAlbedo;    break;
        }

        const auto contains = [&stem](const std::vector<std::string>& list) {
            for (const std::string& token : list) {
                if (stem.find(token) != std::string::npos) return true;
            }
            return false;
        };
        return contains(tokens->want) && !contains(tokens->reject);
    }

    std::string pickBest(const std::vector<fs::path>& files, Role role,
                         const std::vector<std::string>& bases) {
        const fs::path* best = nullptr;
        size_t bestLength = 0;
        for (const fs::path& file : files) {
            if (!isTextureFile(file)) continue;
            const std::string stem = lowerAscii(file.stem().string());
            std::string roleStem = stem;
            size_t bestAt = std::string::npos;
            size_t bestLen = 0;
            for (const std::string& base : bases) {
                if (base.empty()) continue;
                const size_t at = stem.find(base);
                if (at != std::string::npos && base.size() > bestLen) {
                    bestAt = at;
                    bestLen = base.size();
                }
            }
            if (bestAt == std::string::npos) continue;
            roleStem = stem.substr(0, bestAt) + stem.substr(bestAt + bestLen);
            if (!matchesRole(roleStem, role)) continue;
            if (best == nullptr || stem.size() < bestLength) {
                best = &file;
                bestLength = stem.size();
            }
        }
        if (best == nullptr) return "";
        ++discovered;
        return present(*best);
    }

    std::string scanDirectory(const fs::path& dir, Role role,
                              const std::vector<std::string>& bases) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return "";
        std::vector<fs::path> files;
        for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) files.push_back(it->path());
        }
        return pickBest(files, role, bases);
    }

    static std::string lowerAscii(std::string value) {
        for (char& ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    static bool isTextureFile(const fs::path& path) {
        static const std::set<std::string> kExtensions = {
            ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr"};
        return kExtensions.count(lowerAscii(path.extension().string())) > 0;
    }

    static const fs::path* lookup(const std::unordered_map<std::string, fs::path>& table,
                                  const std::string& key) {
        if (key.empty()) return nullptr;
        auto it = table.find(lowerAscii(key));
        return it == table.end() ? nullptr : &it->second;
    }

    void ensureIndex() {
        if (indexBuilt) return;
        indexBuilt = true;
        if (root.empty()) return;

        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        if (ec) return;
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!it->is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }
            const fs::path& path = it->path();
            if (!isTextureFile(path)) continue;
            insertShallowest(byFilename, lowerAscii(path.filename().string()), path);
            insertShallowest(byStem, lowerAscii(path.stem().string()), path);
            allTextures.push_back(path);
        }
    }

    static void insertShallowest(std::unordered_map<std::string, fs::path>& table,
                                 const std::string& key, const fs::path& path) {
        auto existing = table.find(key);
        if (existing == table.end()) {
            table.emplace(key, path);
            return;
        }
        const auto depth = [](const fs::path& p) {
            return std::distance(p.begin(), p.end());
        };
        if (depth(path) < depth(existing->second)) {
            existing->second = path;
        }
    }

    std::string present(const fs::path& path) const {
        std::error_code ec;
        fs::path absolute = fs::weakly_canonical(path, ec);
        if (ec || absolute.empty()) {
            absolute = path;
            ec.clear();
        }
        if (!project.empty()) {
            const fs::path relative = absolute.lexically_relative(project);
            if (!relative.empty() && relative.generic_string() != "." &&
                relative.begin()->string() != "..") {
                return relative.generic_string();
            }
        }
        return absolute.string();
    }

    fs::path root;
    fs::path project;
    bool indexBuilt = false;
    std::unordered_map<std::string, fs::path> byFilename;
    std::unordered_map<std::string, fs::path> byStem;
    std::vector<fs::path> allTextures;
    int foundByName = 0;
    int missing = 0;
    int discovered = 0;
};

float normalizeTimeOfDay(float timeOfDay) {
    if (!std::isfinite(timeOfDay)) {
        return 0.5f;
    }
    float wrapped = std::fmod(timeOfDay, 1.0f);
    if (wrapped < 0.0f) {
        wrapped += 1.0f;
    }
    return wrapped;
}

std::string shaderFilename(const std::string& shaderPath) {
    if (shaderPath.empty()) {
        return std::string();
    }
    return fs::path(shaderPath).filename().string();
}

bool isSupportedVulkanPreviewShaderPair(const SceneObject& obj) {
    const std::string vert = shaderFilename(obj.vertexShaderPath);
    const std::string frag = shaderFilename(obj.fragmentShaderPath);

    const bool vertSupported = vert.empty() ||
                               vert == "vert.glsl" ||
                               vert == "scroll_texture_vert.glsl";
    const bool fragSupported = frag.empty() ||
                               frag == "frag.glsl" ||
                               frag == "scroll_texture_frag.glsl";
    return vertSupported && fragSupported;
}

bool hasUnsupportedVulkanPreviewFeature(const SceneObject& obj) {
    if (!IsObjectEnabledInHierarchy(obj) || !obj.hasRenderer || obj.renderType == RenderType::None) {
        return false;
    }

    const bool supportedGeometry = obj.renderType == RenderType::Cube ||
                                   obj.renderType == RenderType::Plane ||
                                   obj.renderType == RenderType::Mirror;
    if (!supportedGeometry) {
        return true;
    }

    if (!isSupportedVulkanPreviewShaderPair(obj)) {
        return true;
    }

    return false;
}

void ApplyObjectPreset(SceneObject& obj, ObjectType preset) {
    obj.type = preset;
    obj.hasRenderer = false;
    obj.renderType = RenderType::None;
    obj.hasLight = false;
    obj.hasLight2D = false;
    obj.hasReflectionCast = false;
    obj.hasCamera = false;
    obj.hasPostFX = false;
    obj.hasUI = false;
    obj.hasShadowCaster2D = false;
    obj.ui.type = UIElementType::None;

    switch (preset) {
        case ObjectType::Cube:
            obj.hasRenderer = true;
            obj.renderType = RenderType::Cube;
            break;
        case ObjectType::Sphere:
            obj.hasRenderer = true;
            obj.renderType = RenderType::Sphere;
            break;
        case ObjectType::Capsule:
            obj.hasRenderer = true;
            obj.renderType = RenderType::Capsule;
            break;
        case ObjectType::OBJMesh:
            obj.hasRenderer = true;
            obj.renderType = RenderType::OBJMesh;
            break;
        case ObjectType::Model:
            obj.hasRenderer = true;
            obj.renderType = RenderType::Model;
            break;
        case ObjectType::Mirror:
            obj.hasRenderer = true;
            obj.renderType = RenderType::Mirror;
            obj.useOverlay = true;
            obj.material.textureMix = 1.0f;
            obj.material.color = glm::vec3(1.0f);
            obj.scale = glm::vec3(2.0f, 2.0f, 0.05f);
            break;
        case ObjectType::Plane:
            obj.hasRenderer = true;
            obj.renderType = RenderType::Plane;
            obj.scale = glm::vec3(2.0f, 2.0f, 0.05f);
            break;
        case ObjectType::Torus:
            obj.hasRenderer = true;
            obj.renderType = RenderType::Torus;
            break;
        case ObjectType::Sprite:
            obj.hasRenderer = true;
            obj.renderType = RenderType::Sprite;
            obj.scale = glm::vec3(1.0f, 1.0f, 0.05f);
            obj.material.ambientStrength = 1.0f;
            break;
        case ObjectType::Sprite25D:
            obj.hasUI = true;
            obj.ui.type = UIElementType::Sprite2D;
            obj.ui.label = "2.5D Sprite";
            obj.ui.size = glm::vec2(128.0f, 128.0f);
            obj.scale = glm::vec3(1.0f);
            break;
        case ObjectType::ParticleSystem2D:
            obj.hasParticleSystem2D = true;
            obj.particleSystem2D = ParticleSystem2DComponent{};
            obj.scale = glm::vec3(1.0f);
            break;
        case ObjectType::Light2D:
            obj.hasLight2D = true;
            obj.light2D.type = Light2DType::Point;
            obj.light2D.outerRadius = 5.0f;
            obj.light2D.radius = 5.0f;
            obj.light2D.intensity = 1.2f;
            break;
        case ObjectType::ShadowCaster2D:
            obj.hasShadowCaster2D = true;
            obj.shadowCaster2D.points = {
                glm::vec2(-0.5f, -0.5f),
                glm::vec2(0.5f, -0.5f),
                glm::vec2(0.5f, 0.5f),
                glm::vec2(-0.5f, 0.5f)
            };
            break;
        case ObjectType::DirectionalLight:
            obj.hasLight = true;
            obj.light.type = LightType::Directional;
            break;
        case ObjectType::PointLight:
            obj.hasLight = true;
            obj.light.type = LightType::Point;
            obj.light.range = 12.0f;
            obj.light.intensity = 2.0f;
            break;
        case ObjectType::SpotLight:
            obj.hasLight = true;
            obj.light.type = LightType::Spot;
            obj.light.range = 15.0f;
            obj.light.intensity = 2.5f;
            break;
        case ObjectType::AreaLight:
            obj.hasLight = true;
            obj.light.type = LightType::Area;
            obj.light.range = 10.0f;
            obj.light.intensity = 3.0f;
            obj.light.size = glm::vec2(2.0f, 2.0f);
            break;
        case ObjectType::ReflectionCast:
            obj.hasReflectionCast = true;
            obj.reflectionCast = ReflectionCastComponent{};
            obj.scale = obj.reflectionCast.boxSize;
            break;
        case ObjectType::Camera:
            obj.hasCamera = true;
            obj.camera.type = SceneCameraType::Player;
            obj.camera.fov = 60.0f;
            break;
        case ObjectType::PostFXNode:
            obj.hasPostFX = true;
            obj.scale = glm::vec3(6.0f);
            obj.postFx.enabled = true;
            obj.postFx.bloomEnabled = true;
            obj.postFx.colorAdjustEnabled = true;
            break;
        case ObjectType::Canvas:
            obj.hasUI = true;
            obj.ui.type = UIElementType::Canvas;
            obj.ui.label = "Canvas";
            obj.ui.size = glm::vec2(600.0f, 400.0f);
            break;
        case ObjectType::UIImage:
            obj.hasUI = true;
            obj.ui.type = UIElementType::Image;
            obj.ui.label = "Image";
            obj.ui.size = glm::vec2(200.0f, 200.0f);
            break;
        case ObjectType::UISlider:
            obj.hasUI = true;
            obj.ui.type = UIElementType::Slider;
            obj.ui.label = "Slider";
            obj.ui.size = glm::vec2(240.0f, 32.0f);
            break;
        case ObjectType::UIButton:
            obj.hasUI = true;
            obj.ui.type = UIElementType::Button;
            obj.ui.label = "Button";
            obj.ui.size = glm::vec2(160.0f, 40.0f);
            break;
        case ObjectType::UIText:
            obj.hasUI = true;
            obj.ui.type = UIElementType::Text;
            obj.ui.label = "Text";
            obj.ui.size = glm::vec2(240.0f, 32.0f);
            break;
        case ObjectType::Sprite2D:
            obj.hasUI = true;
            obj.ui.type = UIElementType::Sprite2D;
            obj.ui.label = "Sprite2D";
            obj.ui.size = glm::vec2(128.0f, 128.0f);
            break;
        case ObjectType::Empty:
        default:
            break;
    }
}
} // namespace

void ModuRuntime2DRenderCounters_Reset();
void ModuRuntime2DRenderCounters_Read(uint64_t* outTextureBindCount,
                                      uint64_t* outStateBindCount);

namespace {
using Runtime2DClock = std::chrono::steady_clock;

double Runtime2DMsSince(const Runtime2DClock::time_point& start,
                        const Runtime2DClock::time_point& end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

struct Runtime2DImGuiDrawCounters {
    uint64_t drawCalls = 0;
    uint64_t textureBinds = 0;
    uint64_t stateBinds = 0;
};

Runtime2DImGuiDrawCounters CollectImGuiDrawCounters(const ImDrawData* drawData) {
    Runtime2DImGuiDrawCounters counters;
    if (!drawData || drawData->CmdListsCount <= 0) {
        return counters;
    }

    ImTextureID lastTexture = ImTextureID_Invalid;
    bool hasLastTexture = false;
    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
        const ImDrawList* drawList = drawData->CmdLists[listIndex];
        if (!drawList) continue;
        for (const ImDrawCmd& cmd : drawList->CmdBuffer) {
            ++counters.drawCalls;
            ++counters.stateBinds;
            const ImTextureID cmdTexture = cmd.GetTexID();
            if (!hasLastTexture || cmdTexture != lastTexture) {
                ++counters.textureBinds;
                lastTexture = cmdTexture;
                hasLastTexture = true;
            }
        }
    }
    return counters;
}

struct Runtime2DProfileFrame {
    double totalFrameMs = 0.0;
    double update2DTotalMs = 0.0;
    double physics2DTotalMs = 0.0;
    double broadphaseMs = 0.0;
    double narrowphaseSolveMs = 0.0;
    double transformUpdateMs = 0.0;
    double renderSubmissionMs = 0.0;
    double actualDrawRenderMs = 0.0;
    double uiRuntimeMs = 0.0;
    double spriteBatchBuildMs = 0.0;
    double presentWaitMs = 0.0;
    double fpsCapSleepMs = 0.0;
    uint64_t textureBindCount = 0;
    uint64_t stateBindCount = 0;
    uint64_t drawCallCount = 0;
    uint64_t visibleObjectCount = 0;
    uint64_t collisionPairCandidateCount = 0;
    uint64_t collisionTestCount = 0;
};

struct Runtime2DProfileAggregate {
    Runtime2DProfileFrame sum;
    int frameCount = 0;
    double windowStartSec = -1.0;
};

Runtime2DProfileFrame gRuntime2DProfileFrame;
Runtime2DProfileAggregate gRuntime2DProfileAggregate;
bool gRuntime2DProfileEnabled = false;

void resetRuntime2DFrame() {
    gRuntime2DProfileFrame = Runtime2DProfileFrame{};
}

void accumulateRuntime2DFrame() {
    Runtime2DProfileAggregate& agg = gRuntime2DProfileAggregate;
    agg.frameCount += 1;
    agg.sum.totalFrameMs += gRuntime2DProfileFrame.totalFrameMs;
    agg.sum.update2DTotalMs += gRuntime2DProfileFrame.update2DTotalMs;
    agg.sum.physics2DTotalMs += gRuntime2DProfileFrame.physics2DTotalMs;
    agg.sum.broadphaseMs += gRuntime2DProfileFrame.broadphaseMs;
    agg.sum.narrowphaseSolveMs += gRuntime2DProfileFrame.narrowphaseSolveMs;
    agg.sum.transformUpdateMs += gRuntime2DProfileFrame.transformUpdateMs;
    agg.sum.renderSubmissionMs += gRuntime2DProfileFrame.renderSubmissionMs;
    agg.sum.actualDrawRenderMs += gRuntime2DProfileFrame.actualDrawRenderMs;
    agg.sum.uiRuntimeMs += gRuntime2DProfileFrame.uiRuntimeMs;
    agg.sum.spriteBatchBuildMs += gRuntime2DProfileFrame.spriteBatchBuildMs;
    agg.sum.presentWaitMs += gRuntime2DProfileFrame.presentWaitMs;
    agg.sum.fpsCapSleepMs += gRuntime2DProfileFrame.fpsCapSleepMs;
    agg.sum.textureBindCount += gRuntime2DProfileFrame.textureBindCount;
    agg.sum.stateBindCount += gRuntime2DProfileFrame.stateBindCount;
    agg.sum.drawCallCount += gRuntime2DProfileFrame.drawCallCount;
    agg.sum.visibleObjectCount += gRuntime2DProfileFrame.visibleObjectCount;
    agg.sum.collisionPairCandidateCount += gRuntime2DProfileFrame.collisionPairCandidateCount;
    agg.sum.collisionTestCount += gRuntime2DProfileFrame.collisionTestCount;
}

void resetRuntime2DAggregate(double nowSec) {
    gRuntime2DProfileAggregate = Runtime2DProfileAggregate{};
    gRuntime2DProfileAggregate.windowStartSec = nowSec;
}

void emitRuntime2DProfileSummaryIfReady(double nowSec) {
    Runtime2DProfileAggregate& agg = gRuntime2DProfileAggregate;
    if (agg.frameCount <= 0) return;
    if (agg.windowStartSec < 0.0) {
        agg.windowStartSec = nowSec;
    }
    const double elapsedSec = std::max(0.0, nowSec - agg.windowStartSec);
    if (agg.frameCount < 60 && elapsedSec < 1.0) {
        return;
    }

    const double inv = 1.0 / static_cast<double>(agg.frameCount);
    Runtime2DProfileFrame avg;
    avg.totalFrameMs = agg.sum.totalFrameMs * inv;
    avg.update2DTotalMs = agg.sum.update2DTotalMs * inv;
    avg.physics2DTotalMs = agg.sum.physics2DTotalMs * inv;
    avg.broadphaseMs = agg.sum.broadphaseMs * inv;
    avg.narrowphaseSolveMs = agg.sum.narrowphaseSolveMs * inv;
    avg.transformUpdateMs = agg.sum.transformUpdateMs * inv;
    avg.renderSubmissionMs = agg.sum.renderSubmissionMs * inv;
    avg.actualDrawRenderMs = agg.sum.actualDrawRenderMs * inv;
    avg.uiRuntimeMs = agg.sum.uiRuntimeMs * inv;
    avg.spriteBatchBuildMs = agg.sum.spriteBatchBuildMs * inv;
    avg.presentWaitMs = agg.sum.presentWaitMs * inv;
    avg.fpsCapSleepMs = agg.sum.fpsCapSleepMs * inv;
    avg.textureBindCount = static_cast<uint64_t>(std::llround(agg.sum.textureBindCount * inv));
    avg.stateBindCount = static_cast<uint64_t>(std::llround(agg.sum.stateBindCount * inv));
    avg.drawCallCount = static_cast<uint64_t>(std::llround(agg.sum.drawCallCount * inv));
    avg.visibleObjectCount = static_cast<uint64_t>(std::llround(agg.sum.visibleObjectCount * inv));
    avg.collisionPairCandidateCount = static_cast<uint64_t>(std::llround(agg.sum.collisionPairCandidateCount * inv));
    avg.collisionTestCount = static_cast<uint64_t>(std::llround(agg.sum.collisionTestCount * inv));

    struct CostEntry {
        const char* name = "";
        double ms = 0.0;
    };
    std::array<CostEntry, 11> costs = {{
        { "update2D", avg.update2DTotalMs },
        { "physics2D", avg.physics2DTotalMs },
        { "broadphase", avg.broadphaseMs },
        { "narrowphase", avg.narrowphaseSolveMs },
        { "transform", avg.transformUpdateMs },
        { "renderSubmit", avg.renderSubmissionMs },
        { "drawRender", avg.actualDrawRenderMs },
        { "uiRuntime", avg.uiRuntimeMs },
        { "spriteBuild", avg.spriteBatchBuildMs },
        { "presentWait", avg.presentWaitMs },
        { "fpsSleep", avg.fpsCapSleepMs }
    }};
    std::sort(costs.begin(), costs.end(), [](const CostEntry& a, const CostEntry& b) {
        return a.ms > b.ms;
    });

    const double cpuUpdateMs = std::max(0.0, avg.update2DTotalMs - avg.physics2DTotalMs);
    const double physicsMs = avg.physics2DTotalMs;
    const double uiMs = avg.uiRuntimeMs;
    const double renderSubmitMs = avg.renderSubmissionMs;
    const double gpuDrawPresentMs = avg.actualDrawRenderMs + avg.presentWaitMs;
    const double syncWaitMs = avg.fpsCapSleepMs;

    std::array<CostEntry, 6> bottleneckGroups = {{
        { "CPU update", cpuUpdateMs },
        { "physics", physicsMs },
        { "UI", uiMs },
        { "render submission", renderSubmitMs },
        { "GPU/draw/present", gpuDrawPresentMs },
        { "synchronization/waiting", syncWaitMs }
    }};
    std::sort(bottleneckGroups.begin(), bottleneckGroups.end(),
              [](const CostEntry& a, const CostEntry& b) { return a.ms > b.ms; });
    const CostEntry& topGroup = bottleneckGroups.front();

    const bool presentLikelyBound = avg.presentWaitMs > 0.3 &&
        avg.presentWaitMs > avg.update2DTotalMs &&
        avg.presentWaitMs > avg.renderSubmissionMs;
    const bool capLikelyBound = avg.fpsCapSleepMs > 0.3;

    std::fprintf(stdout,
        "[2DProfile avg/%df] frame=%.3fms update2D=%.3fms physics=%.3fms (broad=%.3fms narrow=%.3fms) "
        "transform=%.3fms submit=%.3fms draw=%.3fms ui=%.3fms spriteBuild=%.3fms present=%.3fms sleep=%.3fms "
        "drawCalls=%llu texBinds=%llu stateBinds=%llu visible=%llu pairCandidates=%llu collisionTests=%llu\n",
        agg.frameCount,
        avg.totalFrameMs, avg.update2DTotalMs, avg.physics2DTotalMs, avg.broadphaseMs, avg.narrowphaseSolveMs,
        avg.transformUpdateMs, avg.renderSubmissionMs, avg.actualDrawRenderMs, avg.uiRuntimeMs,
        avg.spriteBatchBuildMs, avg.presentWaitMs, avg.fpsCapSleepMs,
        static_cast<unsigned long long>(avg.drawCallCount),
        static_cast<unsigned long long>(avg.textureBindCount),
        static_cast<unsigned long long>(avg.stateBindCount),
        static_cast<unsigned long long>(avg.visibleObjectCount),
        static_cast<unsigned long long>(avg.collisionPairCandidateCount),
        static_cast<unsigned long long>(avg.collisionTestCount));
    std::fprintf(stdout,
        "[2DProfile top5] 1)%s=%.3fms 2)%s=%.3fms 3)%s=%.3fms 4)%s=%.3fms 5)%s=%.3fms | bottleneck=%s%s%s\n",
        costs[0].name, costs[0].ms,
        costs[1].name, costs[1].ms,
        costs[2].name, costs[2].ms,
        costs[3].name, costs[3].ms,
        costs[4].name, costs[4].ms,
        topGroup.name,
        presentLikelyBound ? " (present-bound likely)" : "",
        capLikelyBound ? " (fps-cap sleep detected)" : "");
    std::fflush(stdout);

    resetRuntime2DAggregate(nowSec);
}
} // namespace

void ModuRuntime2DProfiler_RecordUiRuntime(double uiRuntimeMs,
                                           double spriteBatchBuildMs,
                                           uint32_t visibleObjectCount) {
    if (!gRuntime2DProfileEnabled) return;
    gRuntime2DProfileFrame.uiRuntimeMs += uiRuntimeMs;
    gRuntime2DProfileFrame.spriteBatchBuildMs += spriteBatchBuildMs;
    gRuntime2DProfileFrame.visibleObjectCount += static_cast<uint64_t>(visibleObjectCount);
}

void Engine::syncAssemblageProjectRoot() {
    if (projectManager.currentProject.isLoaded) {
        assemblageRuntime.setProjectRoot(projectManager.currentProject.projectPath);
    } else {
        assemblageRuntime.clear();
    }
}

bool Engine::isProject2DPipeline() const {
    if (!projectManager.currentProject.isLoaded) {
        return false;
    }
    return projectManager.currentProject.pipeline == ProjectPipeline::Pipeline2D;
}

bool Engine::isProject25DPipeline() const {
    if (!projectManager.currentProject.isLoaded) {
        return false;
    }
    return projectManager.currentProject.pipeline == ProjectPipeline::Pipeline25D;
}

bool Engine::is2DWorldEditingEnabled() const {
    return uiCanvasPreviewEnabled && (isProject2DPipeline() || uiWorldMode);
}

void Engine::applyProjectPipelineDefaults(bool force) {
    if (!projectManager.currentProject.isLoaded) {
        return;
    }

    syncAssemblageProjectRoot();

    if (isProject2DPipeline()) {
        uiWorldMode = true;
        pixelGridSnapEnabled = true;
        if (force || !useSnap) {
            useSnap = true;
        }
        float step = static_cast<float>(std::max(1, pixelGridSnapStep));
        if (force || snapValue[0] < 1.0f) {
            snapValue[0] = step;
            snapValue[1] = step;
            snapValue[2] = step;
        }
    } else if (isProject25DPipeline()) {
        uiWorldMode = false;
        if (force && !useSnap) {
            useSnap = true;
        }
    } else if (force) {
        uiWorldMode = false;
    }
}

bool Engine::renderTMViewportPass(const Camera& renderCamera,
                                  int width,
                                  int height,
                                  float fovDeg,
                                  float nearPlane,
                                  float farPlane,
                                  unsigned int* outTexture,
                                  Modularity::Render25D::TMRenderer::RenderStats* outStats,
                                  std::string* outError,
                                  int previewSlot) {
    using namespace Modularity::Render25D;

    unsigned int textureId = 0;
    TMRenderer::RenderStats renderStats;
    std::string error;

    if (width <= 0 || height <= 0) {
        error = "TM renderer received an invalid viewport size";
    } else if (renderCamera.orthographic) {
        error = "TM renderer requires a perspective camera";
    } else {
        TMScene scene;
        TMSceneBuilder::BuildStats buildStats;
        tmSceneBuilder.buildFromSceneObjects(sceneObjects, scene, buildStats);
        (void)buildStats;

        nearPlane = std::max(0.01f, nearPlane);
        farPlane = std::max(nearPlane + 0.01f, farPlane);
        const glm::vec3 cameraForward =
            (glm::dot(renderCamera.front, renderCamera.front) > 1e-6f)
                ? glm::normalize(renderCamera.front)
                : glm::vec3(0.0f, 0.0f, -1.0f);

        TMRenderContext context;
        context.view = renderCamera.getViewMatrix();
        context.projection = glm::perspective(glm::radians(fovDeg),
                                              static_cast<float>(width) /
                                                  static_cast<float>(std::max(1, height)),
                                              nearPlane,
                                              farPlane);
        context.cameraPosition = renderCamera.position;
        context.cameraForward = cameraForward;
        context.viewportWidth = width;
        context.viewportHeight = height;
        context.maxVisibleDistance = std::max(64.0f, farPlane);
        context.skyboxTimeOfDay = sceneTimeOfDay;
        context.skyboxSettings = sceneSkyboxSettings;
        context.presentationSettings = tmOpenGLRenderer.getPresentationSettings();

        const bool ok = [&]() {
            if (previewSlot >= 0) {
                textureId = tmOpenGLRenderer.renderPreview(tmRenderer, context, scene, previewSlot,
                                                           renderStats, error);
                return textureId != 0;
            }
            const bool rendered = tmOpenGLRenderer.renderViewport(tmRenderer, context, scene,
                                                                  renderStats, error);
            textureId = tmOpenGLRenderer.getViewportTexture();
            return rendered && textureId != 0;
        }();
        if (ok) {
            textureId = (previewSlot >= 0) ? textureId : tmOpenGLRenderer.getViewportTexture();
        }
    }

    if (outTexture != nullptr) {
        *outTexture = textureId;
    }
    if (outStats != nullptr) {
        *outStats = renderStats;
    }
    if (outError != nullptr) {
        *outError = error;
    }
    return textureId != 0 && error.empty();
}

unsigned int Engine::getActiveSceneTexture() const {
    if (isProject25DPipeline()) {
        const unsigned int tmTexture = tmOpenGLRenderer.getViewportTexture();
        if (tmTexture != 0) {
            return tmTexture;
        }
    }
    return renderer.getViewportTexture();
}

std::string Engine::buildTMOverlayLabel(const Modularity::Render25D::TMRenderer::RenderStats& stats,
                                        const std::string& error) const {
    if (!error.empty()) {
        return "TM Active | " + error;
    }
    return "TM Active | Seg " + std::to_string(stats.visibleSegments) +
           " | Floor " + std::to_string(stats.floorCommands) +
           " | Models " + std::to_string(stats.modelCommands) +
           " | Cull " + std::to_string(stats.frustumRejectedModels) +
           " | Infl " + std::to_string(stats.presentationBoundsModels) +
           " | Skip " + std::to_string(stats.skippedFrustumCullingModels);
}

void Engine::queueOverlayPostFx(ImDrawList* drawList,
                                const Camera& effectCamera,
                                const ImVec2& min,
                                const ImVec2& max,
                                bool allowHistory,
                                OverlayPostFxKind kind) {
    if (drawList == nullptr || usingVulkan() || !rendererInitialized ||
        max.x <= min.x || max.y <= min.y) {
        return;
    }

    overlayPostFxRequests.push_back(
        OverlayPostFxRequest{this, effectCamera, min, max, allowHistory, kind});
    drawList->AddCallback(applyOverlayPostFxCallback,
                          &overlayPostFxRequests.back());
    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

namespace {
void RestoreImGuiTextureUnit() {
    glActiveTexture(GL_TEXTURE0);
}
} // namespace

void Engine::applyOverlayPostFxCallback(const ImDrawList*,
                                        const ImDrawCmd* command) {
    if (command == nullptr || command->UserCallbackData == nullptr) {
        return;
    }
    auto* request =
        static_cast<OverlayPostFxRequest*>(command->UserCallbackData);
    if (request->engine == nullptr) {
        return;
    }

    const ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr) {
        return;
    }

    const float scaleX = std::max(0.01f, drawData->FramebufferScale.x);
    const float scaleY = std::max(0.01f, drawData->FramebufferScale.y);
    const int framebufferWidth =
        static_cast<int>(std::round(drawData->DisplaySize.x * scaleX));
    const int framebufferHeight =
        static_cast<int>(std::round(drawData->DisplaySize.y * scaleY));
    const int x0 = std::clamp(
        static_cast<int>(std::floor(
            (request->min.x - drawData->DisplayPos.x) * scaleX)),
        0, framebufferWidth);
    const int x1 = std::clamp(
        static_cast<int>(std::ceil(
            (request->max.x - drawData->DisplayPos.x) * scaleX)),
        0, framebufferWidth);
    const int y0 = std::clamp(
        framebufferHeight -
            static_cast<int>(std::ceil(
                (request->max.y - drawData->DisplayPos.y) * scaleY)),
        0, framebufferHeight);
    const int y1 = std::clamp(
        framebufferHeight -
            static_cast<int>(std::floor(
                (request->min.y - drawData->DisplayPos.y) * scaleY)),
        0, framebufferHeight);
    if (x1 <= x0 || y1 <= y0) {
        if (request->kind == OverlayPostFxKind::BandEnd) {
            request->engine->renderer.endBandPostFxRegion(
                request->camera, request->engine->sceneObjects,
                request->allowHistory);
            RestoreImGuiTextureUnit();
        }
        return;
    }

    Renderer& renderer = request->engine->renderer;
    switch (request->kind) {
        case OverlayPostFxKind::BandBegin:
            renderer.beginBandPostFxRegion(x0, y0, x1 - x0, y1 - y0,
                                           framebufferWidth, framebufferHeight);
            break;
        case OverlayPostFxKind::BandEnd:
            renderer.endBandPostFxRegion(request->camera,
                                         request->engine->sceneObjects,
                                         request->allowHistory);
            break;
        case OverlayPostFxKind::FullRegion:
        default:
            renderer.postProcessFramebufferRegion(
                request->camera, request->engine->sceneObjects,
                x0, y0, x1 - x0, y1 - y0, request->allowHistory);
            break;
    }

    RestoreImGuiTextureUnit();
}

namespace {
bool HasMeaningfulSpriteFrameScales(const UIElementComponent& ui) {
    if (ui.spriteCustomFrameScales.size() != ui.spriteCustomFrames.size() ||
        ui.spriteCustomFrameScales.empty()) {
        return false;
    }
    for (const glm::vec2& scale : ui.spriteCustomFrameScales) {
        if (std::abs(scale.x - 1.0f) > 0.0001f || std::abs(scale.y - 1.0f) > 0.0001f) {
            return true;
        }
    }
    return false;
}

glm::vec2 ResolveSpriteFrameScale(const UIElementComponent& ui, int frameIndex) {
    if (!ui.spriteCustomFramesEnabled || ui.spriteCustomFrames.empty()) {
        return glm::vec2(1.0f);
    }

    const int frameCount = static_cast<int>(ui.spriteCustomFrames.size());
    const int frame = std::clamp(frameIndex, 0, frameCount - 1);
    if (HasMeaningfulSpriteFrameScales(ui)) {
        const glm::vec2 authored = ui.spriteCustomFrameScales[static_cast<size_t>(frame)];
        return glm::vec2(std::max(0.01f, authored.x), std::max(0.01f, authored.y));
    }

    const glm::ivec4& referenceRect = ui.spriteCustomFrames.front();
    const glm::ivec4& frameRect = ui.spriteCustomFrames[static_cast<size_t>(frame)];
    const float referenceWidth = static_cast<float>(std::max(1, referenceRect.z));
    const float referenceHeight = static_cast<float>(std::max(1, referenceRect.w));
    return glm::vec2(
        static_cast<float>(std::max(1, frameRect.z)) / referenceWidth,
        static_cast<float>(std::max(1, frameRect.w)) / referenceHeight);
}
}

int Engine::resolveSpriteSheetFrame(const SceneObject& obj) const {
    if (obj.ui.spriteCustomFramesEnabled && !obj.ui.spriteCustomFrames.empty()) {
        int total = static_cast<int>(obj.ui.spriteCustomFrames.size());
        int frame = std::max(0, obj.ui.spriteSheetFrame);
        return frame % total;
    }
    if (!obj.ui.spriteSheetEnabled) {
        return 0;
    }
    int columns = std::max(1, obj.ui.spriteSheetColumns);
    int rows = std::max(1, obj.ui.spriteSheetRows);
    int total = std::max(1, columns * rows);
    int frame = std::max(0, obj.ui.spriteSheetFrame);
    return frame % total;
}

glm::vec2 Engine::getSpriteDisplaySize(const SceneObject& obj) const {
    glm::vec2 size(std::max(0.01f, obj.ui.size.x), std::max(0.01f, obj.ui.size.y));
    if (obj.hasUI && obj.ui.type != UIElementType::None) {
        size.x *= std::max(0.01f, std::abs(obj.scale.x));
        size.y *= std::max(0.01f, std::abs(obj.scale.y));
    }
    // Keep frame-to-frame clip scale from bullying the object's authored size.
    return size * ResolveSpriteFrameScale(obj.ui, resolveSpriteSheetFrame(obj));
}

std::array<ImVec2, 4> Engine::buildSpriteSheetUvs(const SceneObject& obj) const {
    std::array<ImVec2, 4> uvs = {
        ImVec2(0.0f, 1.0f),
        ImVec2(1.0f, 1.0f),
        ImVec2(1.0f, 0.0f),
        ImVec2(0.0f, 0.0f)
    };
    if (!obj.ui.spriteSheetEnabled) {
        return uvs;
    }

    if (obj.ui.spriteCustomFramesEnabled &&
        !obj.ui.spriteCustomFrames.empty() &&
        obj.ui.spriteSourceWidth > 0 &&
        obj.ui.spriteSourceHeight > 0) {
        const glm::ivec4 rect = obj.ui.spriteCustomFrames[resolveSpriteSheetFrame(obj)];
        const float invW = 1.0f / static_cast<float>(obj.ui.spriteSourceWidth);
        const float invH = 1.0f / static_cast<float>(obj.ui.spriteSourceHeight);
        const float u0 = rect.x * invW;
        const float u1 = (rect.x + rect.z) * invW;
        const float vTop = 1.0f - rect.y * invH;
        const float vBottom = 1.0f - (rect.y + rect.w) * invH;
        uvs[0] = ImVec2(u0, vTop);
        uvs[1] = ImVec2(u1, vTop);
        uvs[2] = ImVec2(u1, vBottom);
        uvs[3] = ImVec2(u0, vBottom);
        return uvs;
    }

    int columns = std::max(1, obj.ui.spriteSheetColumns);
    int rows = std::max(1, obj.ui.spriteSheetRows);
    int frame = resolveSpriteSheetFrame(obj);
    int col = frame % columns;
    int row = frame / columns;

    float u0 = static_cast<float>(col) / static_cast<float>(columns);
    float u1 = static_cast<float>(col + 1) / static_cast<float>(columns);
    float vTop = 1.0f - static_cast<float>(row) / static_cast<float>(rows);
    float vBottom = 1.0f - static_cast<float>(row + 1) / static_cast<float>(rows);

    uvs[0] = ImVec2(u0, vTop);
    uvs[1] = ImVec2(u1, vTop);
    uvs[2] = ImVec2(u1, vBottom);
    uvs[3] = ImVec2(u0, vBottom);
    return uvs;
}

namespace {
RawMeshAsset buildCubeRMesh() {
    RawMeshAsset mesh;
    mesh.positions.reserve(24);
    mesh.normals.reserve(24);
    mesh.uvs.reserve(24);
    mesh.faces.reserve(12);

    const float h = 0.5f;
    auto pushFace = [&](const glm::vec3& n, const glm::vec3& uAxis, const glm::vec3& vAxis,
                        const glm::vec3& v0, const glm::vec3& v1,
                        const glm::vec3& v2, const glm::vec3& v3) {
        uint32_t base = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(v0);
        mesh.positions.push_back(v1);
        mesh.positions.push_back(v2);
        mesh.positions.push_back(v3);
        mesh.normals.push_back(n);
        mesh.normals.push_back(n);
        mesh.normals.push_back(n);
        mesh.normals.push_back(n);
        auto toUv = [&](const glm::vec3& p) -> glm::vec2 {
            float u = glm::dot(p, uAxis) / (2.0f * h) + 0.5f;
            float v = glm::dot(p, vAxis) / (2.0f * h) + 0.5f;
            return glm::vec2(u, v);
        };
        mesh.uvs.push_back(toUv(v0));
        mesh.uvs.push_back(toUv(v1));
        mesh.uvs.push_back(toUv(v2));
        mesh.uvs.push_back(toUv(v3));
        mesh.faces.push_back(glm::u32vec3(base, base + 1, base + 2));
        mesh.faces.push_back(glm::u32vec3(base, base + 2, base + 3));
    };

    // +Z (front)
    pushFace(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
             glm::vec3(-h, -h,  h), glm::vec3( h, -h,  h),
             glm::vec3( h,  h,  h), glm::vec3(-h,  h,  h));
    // -Z (back)
    pushFace(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
             glm::vec3( h, -h, -h), glm::vec3(-h, -h, -h),
             glm::vec3(-h,  h, -h), glm::vec3( h,  h, -h));
    // -X (left)
    pushFace(glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f),
             glm::vec3(-h, -h, -h), glm::vec3(-h, -h,  h),
             glm::vec3(-h,  h,  h), glm::vec3(-h,  h, -h));
    // +X (right)
    pushFace(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f),
             glm::vec3( h, -h,  h), glm::vec3( h, -h, -h),
             glm::vec3( h,  h, -h), glm::vec3( h,  h,  h));
    // +Y (top)
    pushFace(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
             glm::vec3(-h,  h,  h), glm::vec3( h,  h,  h),
             glm::vec3( h,  h, -h), glm::vec3(-h,  h, -h));
    // -Y (bottom)
    pushFace(glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
             glm::vec3(-h, -h, -h), glm::vec3( h, -h, -h),
             glm::vec3( h, -h,  h), glm::vec3(-h, -h,  h));

    mesh.boundsMin = glm::vec3(-h);
    mesh.boundsMax = glm::vec3(h);
    mesh.hasNormals = true;
    mesh.hasUVs = true;
    return mesh;
}

RawMeshAsset buildPlaneRMesh() {
    RawMeshAsset mesh;
    mesh.positions = {
        glm::vec3(-0.5f, 0.0f,  0.5f),
        glm::vec3( 0.5f, 0.0f,  0.5f),
        glm::vec3( 0.5f, 0.0f, -0.5f),
        glm::vec3(-0.5f, 0.0f, -0.5f),
    };
    mesh.normals = {
        glm::vec3(0, 1, 0),
        glm::vec3(0, 1, 0),
        glm::vec3(0, 1, 0),
        glm::vec3(0, 1, 0),
    };
    mesh.uvs = {
        glm::vec2(0, 0),
        glm::vec2(1, 0),
        glm::vec2(1, 1),
        glm::vec2(0, 1),
    };
    mesh.faces = {
        glm::u32vec3(0, 1, 2),
        glm::u32vec3(0, 2, 3),
    };
    mesh.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
    mesh.boundsMax = glm::vec3(0.5f, 0.0f, 0.5f);
    mesh.hasNormals = true;
    mesh.hasUVs = true;
    return mesh;
}

RawMeshAsset buildSphereRMesh(int slices = 24, int stacks = 16) {
    RawMeshAsset mesh;
    const float radius = 0.5f;
    for (int i = 0; i <= stacks; ++i) {
        float v = static_cast<float>(i) / static_cast<float>(stacks);
        float phi = v * glm::pi<float>();
        float y = std::cos(phi);
        float r = std::sin(phi);
        for (int j = 0; j <= slices; ++j) {
            float u = static_cast<float>(j) / static_cast<float>(slices);
            float theta = u * glm::two_pi<float>();
            float x = r * std::cos(theta);
            float z = r * std::sin(theta);
            glm::vec3 pos = glm::vec3(x, y, z) * radius;
            mesh.positions.push_back(pos);
            mesh.normals.push_back(glm::normalize(glm::vec3(x, y, z)));
            mesh.uvs.push_back(glm::vec2(u, 1.0f - v));
        }
    }

    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            uint32_t i0 = i * (slices + 1) + j;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + (slices + 1);
            uint32_t i3 = i2 + 1;
            mesh.faces.push_back(glm::u32vec3(i0, i1, i2));
            mesh.faces.push_back(glm::u32vec3(i1, i3, i2));
        }
    }

    mesh.boundsMin = glm::vec3(-radius);
    mesh.boundsMax = glm::vec3(radius);
    mesh.hasNormals = true;
    mesh.hasUVs = true;
    return mesh;
}

} // namespace
#pragma endregion

namespace { bool runCommandCapture(const std::string& command, std::string& output); }

bool Engine::runToolCommand(const std::string& command, std::string& output) const {
    return runCommandCapture(command, output);
}

#pragma region Build Helpers
namespace {
bool runCommandCapture(const std::string& command, std::string& output) {
    std::array<char, 256> buffer{};
#ifdef _WIN32
    // cmd /c consumes an outer pair of quotes. Keep quotes around executable
    // paths (notably a JDK under Program Files) intact inside that pair.
    const std::string shellCommand = "\"" + command + "\"";
    FILE* pipe = _popen(shellCommand.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        output = "Failed to spawn process: " + command;
        return false;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

#ifdef _WIN32
    int returnCode = _pclose(pipe);
#else
    int returnCode = pclose(pipe);
#endif
    if (returnCode != 0) {
        return false;
    }
    return true;
}

bool runCommandStreaming(const std::string& command,
                         const std::function<void(const std::string&)>& onChunk,
                         int* exitCodeOut) {
    std::array<char, 256> buffer{};
#ifdef _WIN32
    const std::string shellCommand = "\"" + command + "\"";
    FILE* pipe = _popen(shellCommand.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        if (exitCodeOut) *exitCodeOut = -1;
        return false;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        if (onChunk) {
            onChunk(buffer.data());
        }
    }

#ifdef _WIN32
    int returnCode = _pclose(pipe);
#else
    int returnCode = pclose(pipe);
#endif
    if (exitCodeOut) *exitCodeOut = returnCode;
    return returnCode == 0;
}

std::string escapeShellArg(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '"') {
            escaped += "\\\"";
        } else {
            escaped.push_back(c);
        }
    }
    return escaped;
}

std::vector<std::string> splitLines(const std::string& value) {
    std::vector<std::string> lines;
    std::stringstream ss(value);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), required);
    return wide;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), required, nullptr, nullptr);
    return utf8;
}

bool crackHttpUrl(const std::string& url,
                  std::wstring& hostOut,
                  std::wstring& pathOut,
                  INTERNET_PORT& portOut,
                  bool& secureOut) {
    std::wstring wideUrl = utf8ToWide(url);
    if (wideUrl.empty()) {
        return false;
    }

    URL_COMPONENTS components{};
    wchar_t hostBuffer[512] = {};
    wchar_t pathBuffer[2048] = {};
    wchar_t extraBuffer[2048] = {};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = hostBuffer;
    components.dwHostNameLength = static_cast<DWORD>(std::size(hostBuffer));
    components.lpszUrlPath = pathBuffer;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuffer));
    components.lpszExtraInfo = extraBuffer;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extraBuffer));

    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
        return false;
    }

    hostOut.assign(components.lpszHostName, components.dwHostNameLength);
    pathOut.assign(components.lpszUrlPath, components.dwUrlPathLength);
    pathOut.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (pathOut.empty()) {
        pathOut = L"/";
    }
    portOut = components.nPort;
    secureOut = components.nScheme == INTERNET_SCHEME_HTTPS;
    return !hostOut.empty();
}

std::string httpPostWindows(const std::string& url,
                            const std::string& contentType,
                            const std::string& body,
                            const std::string& headers,
                            const std::function<bool(const std::string&)>& onChunk = {},
                            bool* outSuccess = nullptr) {
    if (outSuccess) *outSuccess = false;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
    if (!crackHttpUrl(url, host, path, port, secure)) {
        return "Failed to parse URL: " + url;
    }

    HINTERNET session = WinHttpOpen(L"Modularity/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        return "WinHttpOpen failed";
    }

    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return "WinHttpConnect failed";
    }

    const DWORD requestFlags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection,
                                           L"POST",
                                           path.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           requestFlags);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return "WinHttpOpenRequest failed";
    }

    std::wstring wideHeaders;
    if (!contentType.empty()) {
        wideHeaders += L"Content-Type: " + utf8ToWide(contentType) + L"\r\n";
    }
    for (const std::string& header : splitLines(headers)) {
        wideHeaders += utf8ToWide(header) + L"\r\n";
    }

    BOOL sendOk = WinHttpSendRequest(request,
                                     wideHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wideHeaders.c_str(),
                                     wideHeaders.empty() ? 0 : static_cast<DWORD>(wideHeaders.size()),
                                     body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                     static_cast<DWORD>(body.size()),
                                     static_cast<DWORD>(body.size()),
                                     0);
    if (!sendOk) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return "WinHttpSendRequest failed";
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return "WinHttpReceiveResponse failed";
    }

    bool transportOk = true;
    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            transportOk = false;
            response = "WinHttpQueryDataAvailable failed";
            break;
        }
        if (available == 0) {
            break;
        }

        std::string chunk(static_cast<size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            transportOk = false;
            response = "WinHttpReadData failed";
            break;
        }
        chunk.resize(static_cast<size_t>(read));
        if (onChunk && !chunk.empty()) {
            if (!onChunk(chunk)) {
                transportOk = false;
                response = "HTTP request cancelled";
                break;
            }
        }
        response += chunk;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    const bool haveStatus = WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode,
                            &statusCodeSize,
                            WINHTTP_NO_HEADER_INDEX) != FALSE;
    if (outSuccess) *outSuccess = transportOk && haveStatus && statusCode >= 200 && statusCode < 400;
    if (haveStatus && statusCode >= 400) {
        response = "HTTP " + std::to_string(statusCode) + "\n" + response;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}
#endif

fs::path resolveExecutablePath(const fs::path& buildRoot, const char* exeBaseName) {
#ifdef _WIN32
    std::string exeName = std::string(exeBaseName) + ".exe";
#else
    std::string exeName = exeBaseName;
#endif

    std::vector<fs::path> candidates;
    candidates.push_back(buildRoot / exeName);
    candidates.push_back(buildRoot / "Release" / exeName);
    candidates.push_back(buildRoot / "RelWithDebInfo" / exeName);
    candidates.push_back(buildRoot / "MinSizeRel" / exeName);
    candidates.push_back(buildRoot / "Debug" / exeName);

    for (const auto& path : candidates) {
        if (fs::exists(path)) return path;
    }

    for (const auto& entry : fs::recursive_directory_iterator(buildRoot)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().filename() == exeName) return entry.path();
    }
    return {};
}

bool isModularitySourceRoot(const fs::path& candidate) {
    std::error_code ec;
    if (!fs::exists(candidate / "CMakeLists.txt", ec)) return false;
    if (!fs::is_directory(candidate / "src", ec)) return false;
    if (!fs::is_directory(candidate / "Resources", ec)) return false;

    std::ifstream cmakeFile(candidate / "CMakeLists.txt");
    std::string line;
    while (std::getline(cmakeFile, line)) {
        if (line.find("project(Modularity") != std::string::npos) {
            return true;
        }
    }
    return false;
}

fs::path findCMakeSourceRoot(const fs::path& start) {
    std::error_code ec;
    fs::path cur = fs::absolute(start, ec);
    if (ec) return {};
    while (!cur.empty()) {
        if (isModularitySourceRoot(cur)) return cur;
        fs::path childRoot = cur / "Modularity";
        if (isModularitySourceRoot(childRoot)) return childRoot;
        if (!cur.has_parent_path()) break;
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

bool copyDirectoryRecursive(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    if (!fs::exists(from)) return true;
    fs::create_directories(to, ec);
    if (ec) {
        error = "Failed to create directory: " + to.string();
        return false;
    }

    for (const auto& entry : fs::recursive_directory_iterator(from)) {
        const auto& src = entry.path();
        fs::path rel = fs::relative(src, from, ec);
        if (ec) {
            error = "Failed to resolve relative path: " + src.string();
            return false;
        }
        fs::path dst = to / rel;
        if (entry.is_directory()) {
            fs::create_directories(dst, ec);
            if (ec) {
                error = "Failed to create directory: " + dst.string();
                return false;
            }
        } else if (entry.is_regular_file()) {
            fs::create_directories(dst.parent_path(), ec);
            if (ec) {
                error = "Failed to create directory: " + dst.parent_path().string();
                return false;
            }
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = "Failed to copy file: " + src.string();
                return false;
            }
        }
    }
    return true;
}

fs::path resolveRecentProjectRoot(const std::string& recentPath) {
    fs::path path(recentPath);
    if (path.empty()) return {};
    if (path.extension() == ".modu") {
        return path.parent_path();
    }
    if (fs::is_directory(path)) {
        return path;
    }
    return path.has_parent_path() ? path.parent_path() : path;
}

bool importInstalledPackagesFromRecent(const ProjectManager& projectManager,
                                       const fs::path& targetProjectRoot,
                                       std::string& outSourceProjectName,
                                       std::string& outError) {
    std::error_code ec;
    fs::path targetCanonical = fs::weakly_canonical(targetProjectRoot, ec);
    if (ec || targetCanonical.empty()) {
        ec.clear();
        targetCanonical = fs::absolute(targetProjectRoot, ec);
        ec.clear();
    }

    for (const auto& rp : projectManager.recentProjects) {
        fs::path sourceRoot = resolveRecentProjectRoot(rp.path);
        if (sourceRoot.empty()) continue;

        fs::path sourceCanonical = fs::weakly_canonical(sourceRoot, ec);
        if (ec || sourceCanonical.empty()) {
            ec.clear();
            sourceCanonical = fs::absolute(sourceRoot, ec);
            ec.clear();
        }
        if (!sourceCanonical.empty() && !targetCanonical.empty() && sourceCanonical == targetCanonical) {
            continue;
        }

        fs::path sourceManifest = sourceRoot / "packages.modu";
        if (!fs::exists(sourceManifest)) continue;

        fs::path targetManifest = targetProjectRoot / "packages.modu";
        fs::copy_file(sourceManifest, targetManifest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            outError = "Failed to copy package manifest from " + sourceRoot.string() + ": " + ec.message();
            return false;
        }

        std::string copyError;
        if (!copyDirectoryRecursive(sourceRoot / "Library" / "InstalledPackages",
                                    targetProjectRoot / "Library" / "InstalledPackages",
                                    copyError)) {
            outError = copyError;
            return false;
        }

        outSourceProjectName = rp.name.empty() ? sourceRoot.filename().string() : rp.name;
        return true;
    }

    outError = "No recent project with a package manifest was found.";
    return false;
}

bool applyTemplateProject(const fs::path& templateProjectRoot,
                          Project& project,
                          const std::string& projectName,
                          ProjectPipeline pipeline,
                          Modularity::GraphicsBackend rendererBackend,
                          std::string& outError) {
    if (templateProjectRoot.empty()) {
        return true;
    }

    fs::path templateProjectFile = templateProjectRoot / "project.modu";
    if (!fs::exists(templateProjectFile)) {
        outError = "Template is missing project.modu: " + templateProjectRoot.string();
        return false;
    }

    std::string copyError;
    if (!copyDirectoryRecursive(templateProjectRoot, project.projectPath, copyError)) {
        outError = "Failed to copy template project: " + copyError;
        return false;
    }

    if (!project.load(project.projectPath / "project.modu")) {
        outError = "Failed to load copied template project.";
        return false;
    }

    project.name = projectName;
    project.pipeline = pipeline;
    project.rendererBackend = rendererBackend;
    if (project.currentSceneName.empty()) {
        project.currentSceneName = "Main";
    }
    project.saveProjectFile();
    return true;
}

bool copyPrecompiledPackages(const fs::path& buildRoot, const fs::path& outDir, bool leanRuntimeExport, std::string& error) {
    std::error_code ec;
    if (!fs::exists(buildRoot)) return true;

    if (fs::exists(outDir)) {
        fs::remove_all(outDir, ec);
        if (ec) {
            error = "Failed to clear package cache: " + outDir.string();
            return false;
        }
    }
    fs::create_directories(outDir, ec);
    if (ec) {
        error = "Failed to create package folder: " + outDir.string();
        return false;
    }

    const std::vector<std::string> exts = leanRuntimeExport
        ? std::vector<std::string>{ ".so", ".dylib", ".dll" }
        : std::vector<std::string>{ ".a", ".so", ".dylib", ".lib", ".dll" };
    for (const auto& entry : fs::recursive_directory_iterator(buildRoot)) {
        if (!entry.is_regular_file()) continue;
        fs::path src = entry.path();
        std::string ext = src.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (std::find(exts.begin(), exts.end(), ext) == exts.end()) {
            continue;
        }

        fs::path dst = outDir / src.filename();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Failed to copy package binary: " + src.string();
            return false;
        }
    }
    return true;
}

bool copyPrecompiledEnginePackages(const fs::path& buildRoot, const fs::path& outDir, bool leanRuntimeExport, std::string& error) {
    std::error_code ec;
    if (!fs::exists(buildRoot)) return true;

    if (fs::exists(outDir)) {
        fs::remove_all(outDir, ec);
        if (ec) {
            error = "Failed to clear engine package cache: " + outDir.string();
            return false;
        }
    }
    fs::create_directories(outDir, ec);
    if (ec) {
        error = "Failed to create engine package folder: " + outDir.string();
        return false;
    }

    auto isEngineLib = [](const std::string& filename) {
        std::string name = filename;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        return name.rfind("libcore", 0) == 0 ||
               name.rfind("core_player", 0) == 0 ||
               name.rfind("core.", 0) == 0 ||
               name.rfind("core_player.", 0) == 0;
    };

    const std::vector<std::string> exts = leanRuntimeExport
        ? std::vector<std::string>{ ".so", ".dylib", ".dll" }
        : std::vector<std::string>{ ".a", ".so", ".dylib", ".lib", ".dll" };
    for (const auto& entry : fs::recursive_directory_iterator(buildRoot)) {
        if (!entry.is_regular_file()) continue;
        fs::path src = entry.path();
        std::string ext = src.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (std::find(exts.begin(), exts.end(), ext) == exts.end()) {
            continue;
        }
        if (!isEngineLib(src.filename().string())) {
            continue;
        }

        fs::path dst = outDir / src.filename();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Failed to copy engine binary: " + src.string();
            return false;
        }
    }
    return true;
}

std::string sanitizeBuildToken(const std::string& value, const char* fallback) {
    std::string out;
    out.reserve(value.size());
    bool lastWasDash = false;
    for (char c : value) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(c);
            lastWasDash = false;
        } else if (c == '.' || c == '_' || c == '-') {
            out.push_back(c);
            lastWasDash = (c == '-');
        } else if (std::isspace(uc)) {
            if (!lastWasDash && !out.empty()) {
                out.push_back('-');
                lastWasDash = true;
            }
        }
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty()) out = fallback;
    return out;
}

std::string quotePath(const fs::path& path) {
    std::string value = path.string();
    size_t pos = 0;
    while ((pos = value.find('"', pos)) != std::string::npos) {
        value.insert(pos, "\\");
        pos += 2;
    }
    return "\"" + value + "\"";
}

std::string xmlEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string makeAndroidPackageSegment(const std::string& value, const char* fallback) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (c == '_' && !out.empty()) {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out.front()))) {
        out = std::string(fallback) + (out.empty() ? "" : ("_" + out));
    }
    return out;
}

std::string makeAndroidPackageName(const std::string& companyName, const std::string& buildName) {
    return "com." +
        makeAndroidPackageSegment(companyName, "company") + "." +
        makeAndroidPackageSegment(buildName, "game");
}

std::string joinStrings(const std::vector<std::string>& values, const char* separator) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out << separator;
        out << values[i];
    }
    return out.str();
}

std::string lowerExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool appendAssimpImporterForExtension(const std::string& ext, std::set<std::string>& importers) {
    if (ext == ".fbx") {
        importers.insert("FBX");
    } else if (ext == ".blend") {
        importers.insert("BLEND");
    } else if (ext == ".gltf" || ext == ".glb") {
        importers.insert("GLTF");
    } else if (ext == ".obj") {
        importers.insert("OBJ");
    } else if (ext == ".dae") {
        importers.insert("COLLADA");
    } else if (ext == ".3ds") {
        importers.insert("3DS");
    } else if (ext == ".stl") {
        importers.insert("STL");
    } else if (ext == ".ply") {
        importers.insert("PLY");
    } else if (ext == ".x") {
        importers.insert("X");
    } else if (ext == ".x3d") {
        importers.insert("X3D");
    } else if (ext == ".rmesh" || ext == ".mmesh") {
        importers.insert("OBJ");
    } else {
        return false;
    }
    return true;
}

void scanTextForAndroidModelImporters(const std::string& text, std::set<std::string>& importers) {
    auto trim = [](std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        auto end = s.find_last_not_of(" \t\r\n");
        if (start == std::string::npos || end == std::string::npos) {
            s.clear();
            return;
        }
        s = s.substr(start, end - start + 1);
    };

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        trim(line);
        if (line.rfind("meshPath=", 0) != 0) continue;
        std::string value = line.substr(9);
        trim(value);
        // The modular scene format quotes its values (meshPath="...fbx"); the legacy flat one
        // does not. Left quoted, the extension reads `.fbx"` and matches no importer.
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (value.empty()) continue;
        appendAssimpImporterForExtension(lowerExtension(value), importers);
    }
}

// A C# script a build scene (or a packaged object) runs is what needs Mono in the player. A .cs
// file merely sitting in Assets does not: Unity leftovers and the managed project's generated
// sources live there too. Scripts are `path=` lines in both scene formats.
bool textReferencesManagedScript(const std::string& text, std::string& firstScript) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, 5, "path=") != 0) continue;
        std::string value = line.substr(start + 5);
        const size_t end = value.find_last_not_of(" \t\r\n");
        value = end == std::string::npos ? std::string() : value.substr(0, end + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!value.empty() && lowerExtension(value) == ".cs") {
            firstScript = value;
            return true;
        }
    }
    return false;
}

// An .ogg is usually Vorbis, which stb_vorbis decodes; only Opus inside Ogg needs libopusfile.
// The Opus identification header sits in the first page, so a short read settles it.
bool isOggOpusFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    char header[96] = {};
    in.read(header, sizeof(header));
    const std::string bytes(header, static_cast<size_t>(in.gcount()));
    return bytes.find("OpusHead") != std::string::npos;
}

// Containers miniaudio cannot open, so AudioSystem falls back to libsndfile for them.
bool isSndfileOnlyAudioExtension(const std::string& ext) {
    static const char* const kExtensions[] = {
        ".aif", ".aiff", ".aifc", ".au", ".snd", ".caf", ".w64", ".rf64", ".voc", ".paf",
        ".svx", ".8svx", ".sd2", ".xi", ".htk", ".wve", ".avr", ".ircam", ".nist", ".sph"};
    for (const char* candidate : kExtensions) {
        if (ext == candidate) return true;
    }
    return false;
}

std::vector<std::string> detectAndroidAssimpImporters(const fs::path& projectRoot,
                                                      const fs::path& scenesPath,
                                                      const std::vector<std::string>& runtimeSceneNames) {
    std::set<std::string> importers;
    for (const std::string& sceneName : runtimeSceneNames) {
        if (sceneName.empty()) continue;
        const fs::path scenePath = scenesPath / (sceneName + ".scene");
        std::ifstream in(scenePath);
        if (!in.is_open()) continue;
        std::ostringstream text;
        text << in.rdbuf();
        scanTextForAndroidModelImporters(text.str(), importers);
    }

    std::error_code ec;
    const fs::path assetsRoot = projectRoot / "Assets";
    if (fs::is_directory(assetsRoot, ec)) {
        for (auto it = fs::recursive_directory_iterator(assetsRoot, ec);
             !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec || !it->is_regular_file()) continue;
            appendAssimpImporterForExtension(lowerExtension(it->path()), importers);
        }
    }

    return std::vector<std::string>(importers.begin(), importers.end());
}

bool writeTextFileForExport(const fs::path& path, const std::string& text, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Failed to create directory: " + path.parent_path().string();
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        error = "Failed to write file: " + path.string();
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

fs::path findExecutableInPath(const std::string& name) {
    const char* rawPath = std::getenv("PATH");
    if (!rawPath || !*rawPath) return {};
#ifdef _WIN32
    constexpr char separator = ';';
    const std::vector<std::string> suffixes = {".exe", ".bat", ".cmd", ""};
#else
    constexpr char separator = ':';
    const std::vector<std::string> suffixes = {""};
#endif
    std::stringstream ss(rawPath);
    std::string dir;
    while (std::getline(ss, dir, separator)) {
        if (dir.empty()) continue;
        for (const std::string& suffix : suffixes) {
            fs::path candidate = fs::path(dir) / (name + suffix);
            std::error_code ec;
            if (fs::exists(candidate, ec) && !ec) {
                return candidate;
            }
        }
    }
    return {};
}

std::string androidToolName(const char* name) {
#ifdef _WIN32
    return std::string(name) + ".exe";
#else
    return name;
#endif
}

fs::path findAndroidSdkRoot() {
    return AndroidToolchain::FindSdkRoot();
}

fs::path findJavaTool(const char* name) {
    auto jdk = AndroidToolchain::EnvironmentPath("JAVA_HOME");
    auto candidate = jdk / "bin" / androidToolName(name);
    if (!jdk.empty() && AndroidToolchain::Exists(candidate)) return candidate;
    if (auto tool = findExecutableInPath(name); !tool.empty()) return tool;
#ifdef _WIN32
    auto programs = AndroidToolchain::EnvironmentPath("ProgramFiles");
    if (!programs.empty()) {
        for (const auto& vendor : {programs / "Java" / "latest", programs / "Java",
                                   programs / "Eclipse Adoptium", programs / "Microsoft"}) {
            std::vector<fs::path> roots{vendor};
            std::error_code ec;
            for (fs::directory_iterator it(vendor, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (it->is_directory(ec) && AndroidToolchain::Exists(it->path() / "bin" / "javac.exe")) roots.push_back(it->path());
            }
            auto version = [](const fs::path& root) {
                const std::string text = root.filename().string();
                const auto start = text.find_first_of("0123456789");
                if (start == std::string::npos) return std::vector<int>{};
                auto end = text.find_first_not_of("0123456789.", start);
                std::string number = text.substr(start, end - start);
                while (!number.empty() && number.back() == '.') number.pop_back();
                return AndroidToolchain::Version(number);
            };
            std::sort(roots.begin(), roots.end(), [&](const fs::path& a, const fs::path& b) { return version(a) > version(b); });
            for (const auto& root : roots) {
                candidate = root / "bin" / androidToolName(name);
                if (AndroidToolchain::Exists(candidate)) return candidate;
            }
        }
    }
#endif
    return {};
}

bool androidCMakeGeneratorArgs(const fs::path& sdk, const fs::path& buildRoot,
                               std::string& arguments, std::string& error) {
    std::error_code ec;
    if (fs::exists(buildRoot / "CMakeCache.txt", ec)) return true;
    auto ninja = AndroidToolchain::FindNinja(sdk);
    if (ninja.empty()) ninja = findExecutableInPath("ninja");
    if (!ninja.empty()) {
        arguments = " -G Ninja -DCMAKE_MAKE_PROGRAM=" + quotePath(ninja);
        return true;
    }
#ifdef _WIN32
    error = "Android cross-compilation on Windows needs Ninja. Run tools/Setup-Android.ps1 or install CMake in the Android SDK Manager.";
    return false;
#else
    (void)error;
    return true;
#endif
}

bool stageAndroidCxxRuntime(const fs::path& ndk, const std::string& abi,
                            const fs::path& libDir, std::string& error) {
    const auto runtime = AndroidToolchain::FindSharedCxxRuntime(ndk, abi);
    if (runtime.empty()) {
        error = "NDK libc++_shared.so not found for " + abi + ". Reinstall the selected NDK.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(libDir, ec);
    if (!ec) fs::copy_file(runtime, libDir / "libc++_shared.so", fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "Failed to package the Android C++ runtime: " + ec.message(); return false; }
    auto hostRoot = runtime;
    for (int i = 0; i < 5; ++i) hostRoot = hostRoot.parent_path();
    const auto notice = hostRoot / "NOTICE";
    if (AndroidToolchain::Exists(notice)) {
        const auto licenseDir = libDir.parent_path().parent_path() / "assets" / "licenses";
        fs::create_directories(licenseDir, ec);
        if (!ec) fs::copy_file(notice, licenseDir / "libcxx-NOTICE.txt", fs::copy_options::overwrite_existing, ec);
        if (ec) { error = "Failed to package the Android C++ runtime notice: " + ec.message(); return false; }
    }
    return true;
}

std::string androidBuildParallelArgs() {
    unsigned jobs = 4;
    if (const char* raw = std::getenv("CMAKE_BUILD_PARALLEL_LEVEL"); raw && *raw) {
        const std::string value(raw);
        if (value.size() < 5 && value.find_first_not_of("0123456789") == std::string::npos) {
            const unsigned requested = static_cast<unsigned>(std::strtoul(raw, nullptr, 10));
            if (requested) jobs = std::min(requested, 64u);
        }
    }
    return " --parallel " + std::to_string(jobs);
}

fs::path findLatestAndroidJar(const fs::path& sdkRoot, int& outApiLevel) {
    outApiLevel = 0;
    fs::path best;
    std::error_code ec;
    const fs::path platformsRoot = sdkRoot / "platforms";
    if (!fs::is_directory(platformsRoot, ec)) return {};
    for (const auto& entry : fs::directory_iterator(platformsRoot, ec)) {
        if (ec || !entry.is_directory()) continue;
        const std::string name = entry.path().filename().string();
        constexpr const char* prefix = "android-";
        if (name.rfind(prefix, 0) != 0) continue;
        int api = std::atoi(name.c_str() + std::strlen(prefix));
        if (api <= outApiLevel) continue;
        fs::path jar = entry.path() / "android.jar";
        if (!fs::exists(jar, ec) || ec) continue;
        outApiLevel = api;
        best = jar;
    }
    return best;
}

fs::path findLatestAndroidJarInRoots(const std::vector<fs::path>& sdkRoots,
                                     fs::path& outSdkRoot,
                                     int& outApiLevel) {
    outSdkRoot.clear();
    outApiLevel = 0;
    fs::path best;
    for (const fs::path& sdkRoot : sdkRoots) {
        if (sdkRoot.empty()) continue;
        int apiLevel = 0;
        fs::path jar = findLatestAndroidJar(sdkRoot, apiLevel);
        if (jar.empty() || apiLevel <= outApiLevel) continue;
        outApiLevel = apiLevel;
        outSdkRoot = sdkRoot;
        best = jar;
    }
    return best;
}

struct AndroidBuildTools {
    fs::path aapt2;
    fs::path zipalign;
    fs::path apksigner;
    fs::path d8;
};

AndroidBuildTools findLatestAndroidBuildTools(const fs::path& sdkRoot) {
    AndroidBuildTools best;
    std::string bestVersion;
    std::error_code ec;
    const fs::path toolsRoot = sdkRoot / "build-tools";
    if (!fs::is_directory(toolsRoot, ec)) return best;
    for (const auto& entry : fs::directory_iterator(toolsRoot, ec)) {
        if (ec || !entry.is_directory()) continue;
        fs::path aapt2 = entry.path() / androidToolName("aapt2");
        fs::path zipalign = entry.path() / androidToolName("zipalign");
        fs::path apksigner = entry.path() / androidToolName("apksigner");
        fs::path d8 = entry.path() / androidToolName("d8");
#ifdef _WIN32
        if (!fs::exists(apksigner, ec)) {
            apksigner = entry.path() / "apksigner.bat";
        }
        if (!fs::exists(d8, ec)) {
            d8 = entry.path() / "d8.bat";
        }
#endif
        if (!fs::exists(aapt2, ec) || !fs::exists(zipalign, ec) || !fs::exists(apksigner, ec)) {
            continue;
        }
        const std::string version = entry.path().filename().string();
        if (AndroidToolchain::Version(version) > AndroidToolchain::Version(bestVersion)) {
            bestVersion = version;
            best = {aapt2, zipalign, apksigner, fs::exists(d8, ec) && !ec ? d8 : fs::path{}};
        }
    }
    return best;
}

fs::path findAndroidLlvmStrip(const fs::path& ndkRoot) {
    const std::vector<std::string> hosts = {
        "linux-x86_64",
        "windows-x86_64",
        "darwin-x86_64",
        "darwin-arm64"
    };
    const std::string stripName = androidToolName("llvm-strip");
    for (const std::string& host : hosts) {
        fs::path candidate =
            ndkRoot / "toolchains" / "llvm" / "prebuilt" / host / "bin" / stripName;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return findExecutableInPath("llvm-strip");
}

fs::path findAndroidSharedLibrary(const fs::path& buildRoot,
                                  const std::string& soName = "libModularityPlayer.so") {
    std::error_code ec;
    const fs::path direct = buildRoot / soName;
    if (fs::exists(direct, ec) && !ec) return direct;
    for (auto it = fs::recursive_directory_iterator(buildRoot, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec) break;
        if (it->is_regular_file() && it->path().filename() == soName) {
            return it->path();
        }
    }
    return {};
}

static bool bundleAndroidToolchain(const fs::path& bundleSrc,
                                   const fs::path& assetsRoot,
                                   const std::string& abi,
                                   const std::function<void(const std::string&)>& log,
                                   std::string& error,
                                   const fs::path& scriptSourceRoot = {}) {
    std::error_code ec;
    if (!fs::is_directory(bundleSrc, ec)) {
        error = "Toolchain bundle directory not found: " + bundleSrc.string();
        return false;
    }
    const fs::path dest = assetsRoot / "toolchain" / abi;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    fs::copy(bundleSrc, dest,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing |
             fs::copy_options::copy_symlinks, ec);
    if (ec) {
        error = "Failed to copy toolchain into APK assets: " + ec.message();
        return false;
    }

    if (!scriptSourceRoot.empty()) {
        int rc = 0;
        const std::string refresh = "cmake -DSRC_ROOT=" + quotePath(scriptSourceRoot) +
            " -DDST_ROOT=" + quotePath(dest / "headers") + " -P " +
            quotePath(scriptSourceRoot / "cmake/CopyScriptSDK.cmake");
        if (!runCommandStreaming(refresh + " 2>&1", log, &rc)) {
            error = "Cannot refresh the bundled script SDK (exit " + std::to_string(rc) + ").";
            return false;
        }
    }
    // Each APK refreshes the extracted SDK, including changed engine headers.
    std::ofstream(dest / "bundle.id", std::ios::binary | std::ios::trunc)
        << std::chrono::system_clock::now().time_since_epoch().count();

    std::ostringstream list;
    uintmax_t totalBytes = 0;
    size_t count = 0;
    std::error_code walkEc;
    for (auto it = fs::recursive_directory_iterator(dest, walkEc);
         it != fs::recursive_directory_iterator(); it.increment(walkEc)) {
        if (walkEc) break;
        if (!it->is_regular_file(walkEc)) continue;
        const fs::path rel = fs::relative(it->path(), dest, walkEc);
        if (walkEc || rel.empty()) continue;
        const std::string relStr = rel.generic_string();
        if (relStr == "files.list") continue;
        const fs::perms perms = fs::status(it->path(), walkEc).permissions();
        const bool exec = ((perms & fs::perms::owner_exec) != fs::perms::none) ||
                          relStr.rfind("bin/", 0) == 0;
        list << (exec ? 'x' : '-') << ' ' << relStr << "\n";
        std::error_code sizeEc;
        totalBytes += fs::file_size(it->path(), sizeEc);
        ++count;
    }

    std::ofstream out(dest / "files.list", std::ios::trunc);
    if (!out) {
        error = "Failed to write toolchain files.list";
        return false;
    }
    if (walkEc) { error = "Cannot enumerate compiler SDK: " + walkEc.message(); return false; }
    out << list.str();
    out.close();
    if (!out) { error = "Cannot finish compiler SDK file list."; return false; }

    if (log) {
        log("[Android] Bundled on-device compiler SDK: " + std::to_string(count) +
            " files, " + std::to_string(totalBytes / (1024 * 1024)) + " MiB.\n");
    }
    return true;
}

static fs::path findEmbeddedAndroidCompilers(const fs::path& sourceRoot) {
    if (const char* root = std::getenv("MODULARITY_EMBEDDED_COMPILERS_ROOT"); root && *root)
        return fs::path(root);
    const fs::path root = ModularityAndroidDir(sourceRoot, "embedded-compilers") / "arm64-v8a";
    return fs::exists(root / "ready.json") ? root : fs::path{};
}

static bool stageEmbeddedAndroidCompilers(const fs::path& root, const fs::path& libDir, std::string& error) {
    std::ifstream manifest(root / "runtime-libraries.txt");
    if (!manifest) { error = "Embedded compiler setup is incomplete: " + root.string(); return false; }
    std::string name;
    size_t count = 0;
    while (std::getline(manifest, name)) {
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (name.empty()) continue;
        if (fs::path(name).filename() != name || name.rfind("lib", 0) != 0 || fs::path(name).extension() != ".so") {
            error = "Invalid compiler runtime library name: " + name; return false;
        }
        std::error_code ec;
        fs::copy_file(root / "lib" / name, libDir / name, fs::copy_options::overwrite_existing, ec);
        if (ec) { error = "Cannot package compiler runtime " + name + ": " + ec.message(); return false; }
        ++count;
    }
    if (!count) { error = "The embedded compiler runtime list is empty."; return false; }
    return true;
}

struct AndroidXrManifestOptions {
    bool enabled = false;
    bool questSupport = false;
    bool handTracking = false;
};

// Declaring RECORD_AUDIO lists "microphone" on the app's permission page even if it is never
// asked for, so an exported game only declares it when one of its scripts uses Microphone.
// Walks the same tree crossCompileAndroidScripts does (the whole project, minus build output),
// so a script outside Assets (old-layout Scripts/) is not compiled in without its permission.
bool projectUsesMicrophone(const fs::path& projectRoot) {
    std::error_code ec;
    if (!fs::is_directory(projectRoot, ec)) return false;
    for (auto it = fs::recursive_directory_iterator(projectRoot, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) continue;
        if (it->is_directory(ec)) {
            const std::string name = it->path().filename().string();
            if (name == "Builds" || name == "build" || name == "Library" || name == "Cache" || name == ".git") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = lowerExtension(it->path());
        if (ext != ".moducpp" && ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".cs") continue;
        if (it->file_size(ec) > 4 * 1024 * 1024 || ec) continue;
        std::ifstream in(it->path(), std::ios::binary);
        if (!in.is_open()) continue;
        std::ostringstream text;
        text << in.rdbuf();
        if (text.str().find("Microphone") != std::string::npos) return true;
    }
    return false;
}

bool writeAndroidManifest(const fs::path& manifestPath,
                          const std::string& packageName,
                          const std::string& appLabel,
                          const std::string& versionName,
                          bool debuggable,
                          const std::string& iconRef,
                          std::string& error,
                          const std::string& libName = "ModularityPlayer",
                          int targetSdkVersion = 34,
                          const std::string& activityName = "android.app.NativeActivity",
                          bool hasJavaCode = false,
                          const AndroidXrManifestOptions& xr = AndroidXrManifestOptions{},
                          bool recordAudio = false) {
    std::ostringstream manifest;
    manifest << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    manifest << "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n";
    manifest << "    package=\"" << xmlEscape(packageName) << "\"\n";
    manifest << "    android:versionCode=\"1\"\n";
    manifest << "    android:versionName=\"" << xmlEscape(versionName) << "\">\n";
    manifest << "    <uses-sdk android:minSdkVersion=\"26\" android:targetSdkVersion=\""
             << targetSdkVersion << "\" />\n";
    manifest << "    <uses-feature android:glEsVersion=\"0x00030000\" android:required=\"true\" />\n";
    manifest << "    <uses-permission android:name=\"android.permission.READ_EXTERNAL_STORAGE\" android:maxSdkVersion=\"32\" />\n";
    manifest << "    <uses-permission android:name=\"android.permission.WRITE_EXTERNAL_STORAGE\" android:maxSdkVersion=\"28\" />\n";
    manifest << "    <uses-permission android:name=\"android.permission.READ_MEDIA_IMAGES\" />\n";
    manifest << "    <uses-permission android:name=\"android.permission.READ_MEDIA_VIDEO\" />\n";
    manifest << "    <uses-permission android:name=\"android.permission.READ_MEDIA_AUDIO\" />\n";
    manifest << "    <uses-permission android:name=\"android.permission.MANAGE_EXTERNAL_STORAGE\" />\n";
    if (recordAudio) {
        manifest << "    <uses-permission android:name=\"android.permission.RECORD_AUDIO\" />\n";
        // RECORD_AUDIO alone implies a required mic, which hides the app from mic-less devices
        // in store filtering; the engine copes without one (Microphone.Level() just reads 0).
        manifest << "    <uses-feature android:name=\"android.hardware.microphone\" android:required=\"false\" />\n";
    }

    if (xr.enabled) {
        manifest << "\n    <!-- OpenXR (generated because this project enables XR) -->\n";
        manifest << "    <uses-permission android:name=\"org.khronos.openxr.permission.OPENXR\" />\n";
        manifest << "    <uses-permission android:name=\"org.khronos.openxr.permission.OPENXR_SYSTEM\" />\n";
        manifest << "    <queries>\n";
        manifest << "        <!-- find the runtime broker -->\n";
        manifest << "        <provider android:authorities=\"org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker\" />\n";
        manifest << "        <!-- let the loader's client side reach the runtime and API layer services -->\n";
        manifest << "        <intent>\n";
        manifest << "            <action android:name=\"org.khronos.openxr.OpenXRRuntimeService\" />\n";
        manifest << "        </intent>\n";
        manifest << "        <intent>\n";
        manifest << "            <action android:name=\"org.khronos.openxr.OpenXRApiLayerService\" />\n";
        manifest << "        </intent>\n";
        manifest << "    </queries>\n";
        if (xr.questSupport) {
            manifest << "    <uses-feature android:name=\"android.hardware.vr.headtracking\""
                        " android:version=\"1\" android:required=\"true\" />\n";
            if (xr.handTracking) {
                manifest << "    <uses-feature android:name=\"oculus.software.handtracking\""
                            " android:required=\"false\" />\n";
                manifest << "    <uses-permission android:name=\"com.oculus.permission.HAND_TRACKING\" />\n";
            }
        }
    }

    manifest << "    <application\n";
    manifest << "        android:label=\"" << xmlEscape(appLabel) << "\"\n";
    manifest << "        android:requestLegacyExternalStorage=\"true\"\n";
    manifest << "        android:debuggable=\"" << (debuggable ? "true" : "false") << "\"\n";
    manifest << "        android:extractNativeLibs=\"true\"\n";
    manifest << "        android:hasCode=\"" << (hasJavaCode ? "true" : "false") << "\"\n";
    manifest << "        android:icon=\"" << iconRef << "\">\n";
    manifest << "        <activity\n";
    manifest << "            android:name=\"" << xmlEscape(activityName) << "\"\n";
    manifest << "            android:configChanges=\"keyboard|keyboardHidden|orientation|screenSize|uiMode\"\n";
    manifest << "            android:exported=\"true\"\n";
    if (xr.enabled) {
        manifest << "            android:launchMode=\"singleTask\"\n";
        manifest << "            android:resizeableActivity=\"false\"\n";
    }
    manifest << "            android:screenOrientation=\"landscape\">\n";
    manifest << "            <meta-data android:name=\"android.app.lib_name\" android:value=\"" << xmlEscape(libName) << "\" />\n";
    if (xr.enabled && xr.questSupport) {
        manifest << "            <meta-data android:name=\"com.oculus.vr.focusaware\" android:value=\"true\" />\n";
    }
    manifest << "            <intent-filter>\n";
    manifest << "                <action android:name=\"android.intent.action.MAIN\" />\n";
    manifest << "                <category android:name=\"android.intent.category.LAUNCHER\" />\n";
    if (xr.enabled) {
        if (xr.questSupport) {
            manifest << "                <category android:name=\"com.oculus.intent.category.VR\" />\n";
        }
        manifest << "                <category android:name=\"org.khronos.openxr.intent.category.IMMERSIVE_HMD\" />\n";
    }
    manifest << "            </intent-filter>\n";
    manifest << "        </activity>\n";
    manifest << "    </application>\n";
    manifest << "</manifest>\n";
    return writeTextFileForExport(manifestPath, manifest.str(), error);
}

constexpr const char* kAndroidBridgeActivityName = "com.modularity.android.ModularityNativeActivity";

bool writeAndroidActivityBridge(const fs::path& javaRoot, std::string& error) {
    static const char* source = R"JAVA(package com.modularity.android;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.provider.Settings;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;

public class ModularityNativeActivity extends NativeActivity {
    private static final int REQUEST_IMPORT_FILES = 6101;
    private static final int REQUEST_STORAGE_PERMISSION = 6102;
    private static final int REQUEST_MICROPHONE_PERMISSION = 6103;

    private static native void nativeOnFilePickerResult(String[] paths, String error, boolean canceled);

    // can we freely touch shared storage? API 30+ = the "All files access" grant; below that the
    // legacy WRITE_EXTERNAL_STORAGE permission is enough (we ship requestLegacyExternalStorage + targetSdk 28).
    public boolean modularityHasStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                return Environment.isExternalStorageManager();
            } catch (Throwable t) {
                return false;
            }
        }
        return checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    // Public Documents dir, e.g. /storage/emulated/0/Documents, so projects can
    // live in Documents/ModularityProjects to mirror the desktop layout.
    public String modularityExternalDocumentsPath() {
        try {
            File docs = Environment.getExternalStoragePublicDirectory(
                    Environment.DIRECTORY_DOCUMENTS);
            return docs == null ? "" : docs.getAbsolutePath();
        } catch (Throwable t) {
            return "";
        }
    }

    // ask for storage access. API 30+ has no in-app dialog so off to system settings they go;
    // older versions get the normal permission prompt. either way the grant lands async.
    public void modularityRequestStorageAccess() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    try {
                        Uri uri = Uri.parse("package:" + getPackageName());
                        startActivity(new Intent(
                                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, uri));
                    } catch (Throwable t) {
                        // some OEMs reject the per-app page; fall back to the global list so the user can still find us.
                        try {
                            startActivity(new Intent(
                                    Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                        } catch (Throwable ignored) {
                        }
                    }
                } else {
                    try {
                        requestPermissions(
                                new String[] { android.Manifest.permission.WRITE_EXTERNAL_STORAGE },
                                REQUEST_STORAGE_PERMISSION);
                    } catch (Throwable ignored) {
                    }
                }
            }
        });
    }

    // RECORD_AUDIO prompt for script microphone input; native polls checkSelfPermission for the answer.
    public void modularityRequestMicrophonePermission() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    requestPermissions(
                            new String[] { android.Manifest.permission.RECORD_AUDIO },
                            REQUEST_MICROPHONE_PERMISSION);
                } catch (Throwable ignored) {
                }
            }
        });
    }

    public void launchModularityFilePicker(final boolean allowMultiple) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                intent.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, allowMultiple);
                try {
                    startActivityForResult(intent, REQUEST_IMPORT_FILES);
                } catch (Throwable t) {
                    nativeOnFilePickerResult(new String[0], messageFor(t), false);
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, final Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_IMPORT_FILES) {
            return;
        }
        if (resultCode != RESULT_OK || data == null) {
            nativeOnFilePickerResult(new String[0], "", true);
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                copyPickerResult(data);
            }
        }, "ModularityFileImport").start();
    }

    private void copyPickerResult(Intent data) {
        ArrayList<Uri> uris = collectUris(data);
        if (uris.isEmpty()) {
            nativeOnFilePickerResult(new String[0], "No files were selected.", false);
            return;
        }

        File root = new File(getCacheDir(), "modularity-imports");
        deleteRecursive(root);
        File session = new File(root, Long.toString(System.currentTimeMillis()));
        if (!session.mkdirs() && !session.isDirectory()) {
            nativeOnFilePickerResult(new String[0], "Could not create import cache directory.", false);
            return;
        }

        ArrayList<String> paths = new ArrayList<String>();
        StringBuilder errors = new StringBuilder();
        ContentResolver resolver = getContentResolver();
        for (int i = 0; i < uris.size(); ++i) {
            Uri uri = uris.get(i);
            try {
                File copied = copyUriToCache(resolver, uri, session, i);
                paths.add(copied.getAbsolutePath());
            } catch (Throwable t) {
                if (errors.length() > 0) {
                    errors.append('\n');
                }
                errors.append(messageFor(t));
            }
        }
        nativeOnFilePickerResult(paths.toArray(new String[paths.size()]),
                                 errors.toString(),
                                 false);
    }

    private ArrayList<Uri> collectUris(Intent data) {
        ArrayList<Uri> uris = new ArrayList<Uri>();
        ClipData clipData = data.getClipData();
        if (clipData != null) {
            for (int i = 0; i < clipData.getItemCount(); ++i) {
                Uri uri = clipData.getItemAt(i).getUri();
                if (uri != null) {
                    uris.add(uri);
                }
            }
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        return uris;
    }

    private File copyUriToCache(ContentResolver resolver, Uri uri, File directory, int index) throws Exception {
        String displayName = sanitizeFileName(queryDisplayName(resolver, uri));
        if (displayName.length() == 0) {
            displayName = "asset_" + index;
        }
        File target = makeUniqueFile(directory, displayName);
        InputStream input = resolver.openInputStream(uri);
        if (input == null) {
            throw new IllegalStateException("Could not open selected file: " + uri);
        }
        try {
            FileOutputStream output = new FileOutputStream(target);
            try {
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = input.read(buffer)) >= 0) {
                    if (read > 0) {
                        output.write(buffer, 0, read);
                    }
                }
            } finally {
                output.close();
            }
        } finally {
            input.close();
        }
        return target;
    }

    private String queryDisplayName(ContentResolver resolver, Uri uri) {
        Cursor cursor = null;
        try {
            cursor = resolver.query(uri, new String[] { OpenableColumns.DISPLAY_NAME },
                                    null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) {
                    String name = cursor.getString(column);
                    if (name != null) {
                        return name;
                    }
                }
            }
        } catch (Throwable ignored) {
        } finally {
            if (cursor != null) {
                cursor.close();
            }
        }
        String fallback = uri.getLastPathSegment();
        return fallback == null ? "" : fallback;
    }

    private String sanitizeFileName(String raw) {
        if (raw == null) {
            return "";
        }
        StringBuilder out = new StringBuilder(raw.length());
        for (int i = 0; i < raw.length(); ++i) {
            char c = raw.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == ' ') {
                out.append(c);
            } else {
                out.append('_');
            }
        }
        String cleaned = out.toString().trim();
        while (cleaned.startsWith(".")) {
            cleaned = cleaned.substring(1);
        }
        return cleaned;
    }

    private File makeUniqueFile(File directory, String fileName) {
        File candidate = new File(directory, fileName);
        if (!candidate.exists()) {
            return candidate;
        }
        int dot = fileName.lastIndexOf('.');
        String stem = dot > 0 ? fileName.substring(0, dot) : fileName;
        String extension = dot > 0 ? fileName.substring(dot) : "";
        for (int i = 1; i < 10000; ++i) {
            candidate = new File(directory, stem + "_" + i + extension);
            if (!candidate.exists()) {
                return candidate;
            }
        }
        return new File(directory, stem + "_" + System.nanoTime() + extension);
    }

    private void deleteRecursive(File file) {
        if (file == null || !file.exists()) {
            return;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursive(child);
                }
            }
        }
        file.delete();
    }

    private static String messageFor(Throwable t) {
        if (t == null) {
            return "Unknown Android file picker error.";
        }
        String message = t.getMessage();
        return message == null || message.length() == 0 ? t.getClass().getSimpleName() : message;
    }
}
)JAVA";

    return writeTextFileForExport(
        javaRoot / "com" / "modularity" / "android" / "ModularityNativeActivity.java",
        source,
        error);
}

bool stageAndroidOpenXrLoader(const fs::path& apkStageRoot,
                              const fs::path& sourceRoot,
                              const std::string& abi,
                              const std::function<void(const std::string&)>& appendLog,
                              std::string& error) {
    std::error_code ec;
    std::vector<fs::path> candidates;

    if (const char* envDir = std::getenv("MODULARITY_OPENXR_LOADER_DIR")) {
        if (*envDir) {
            const fs::path root(envDir);
            candidates.push_back(root / abi / "libopenxr_loader.so");
            candidates.push_back(root / "libopenxr_loader.so");
        }
    }
    candidates.push_back(sourceRoot / "redist" / "openxr" / abi / "libopenxr_loader.so");
    candidates.push_back(sourceRoot / "src" / "ThirdParty" / "openxr" / "lib" / abi /
                         "libopenxr_loader.so");

    fs::path found;
    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate, ec) && !ec) {
            found = candidate;
            break;
        }
        ec.clear();
    }

    if (found.empty()) {
        std::string message =
            "OpenXR is enabled for this project, but no libopenxr_loader.so was found for " +
            abi +
            ".\nModularity vendors the Khronos loader, so this normally means the git-lfs "
            "objects were not fetched (run `git lfs pull`) or the vendored copy was "
            "deleted.\nRe-fetch it with ./tools/fetch-openxr-loader.sh, or provide your own "
            "at one of:\n";
        for (const fs::path& candidate : candidates) {
            message += "  " + candidate.string() + "\n";
        }
        message += "or set MODULARITY_OPENXR_LOADER_DIR to the folder that contains it.";
        error = message;
        return false;
    }

    const fs::path libDir = apkStageRoot / "lib" / abi;
    fs::create_directories(libDir, ec);
    ec.clear();
    const fs::path destination = libDir / "libopenxr_loader.so";
    fs::copy_file(found, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Failed to stage the OpenXR loader into the APK: " + ec.message();
        return false;
    }
    if (appendLog) {
        appendLog("[Android] Packaged OpenXR loader from " + found.string() + "\n");
    }
    return true;
}

bool writeAndroidLauncherIcon(const fs::path& resRoot,
                              const fs::path& sourceRoot,
                              std::string& iconRef,
                              std::string& error) {
    std::error_code ec;
    const fs::path logoPath = sourceRoot / "Resources" / "Engine-Root" / "Modu-Logo.png";
    if (fs::exists(logoPath, ec) && !ec) {
        fs::path iconPath = resRoot / "mipmap-xxxhdpi" / "ic_launcher.png";
        fs::create_directories(iconPath.parent_path(), ec);
        if (ec) {
            error = "Failed to create icon directory.";
            return false;
        }
        fs::copy_file(logoPath, iconPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Failed to copy Android launcher icon.";
            return false;
        }
        iconRef = "@mipmap/ic_launcher";
        return true;
    }

    iconRef = "@drawable/ic_launcher";
    const std::string vectorIcon =
        "<vector xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
        "    android:width=\"48dp\" android:height=\"48dp\"\n"
        "    android:viewportWidth=\"48\" android:viewportHeight=\"48\">\n"
        "    <path android:fillColor=\"#202124\" android:pathData=\"M0,0h48v48h-48z\"/>\n"
        "    <path android:fillColor=\"#7CFFB2\" android:pathData=\"M9,12h8l7,14l7,-14h8v24h-7v-13l-6,13h-4l-6,-13v13h-7z\"/>\n"
        "</vector>\n";
    return writeTextFileForExport(resRoot / "drawable" / "ic_launcher.xml", vectorIcon, error);
}

bool ensureAndroidDebugKeystore(const fs::path& keystorePath,
                                const std::function<void(const std::string&)>& appendLog,
                                std::string& error) {
    std::error_code ec;
    if (fs::exists(keystorePath, ec) && !ec) {
        return true;
    }
    fs::create_directories(keystorePath.parent_path(), ec);
    if (ec) {
        error = "Failed to create Android signing directory.";
        return false;
    }

    fs::path keytool = findJavaTool("keytool");
    if (keytool.empty()) {
        error = "Android debug keystore is missing and keytool was not found in PATH.";
        return false;
    }

    std::string command =
        quotePath(keytool) +
        " -genkeypair -v -keystore " + quotePath(keystorePath) +
        " -storepass android -alias androiddebugkey -keypass android"
        " -keyalg RSA -keysize 2048 -validity 10000"
        " -dname \"CN=Android Debug,O=Android,C=US\"";
    int exitCode = 0;
    if (appendLog) appendLog("Running: " + command);
    if (!runCommandStreaming(command + " 2>&1", appendLog, &exitCode)) {
        error = "Failed to generate Android debug keystore (exit code " + std::to_string(exitCode) + ").";
        return false;
    }
    return true;
}

bool packageAndroidApk(const fs::path& apkStageRoot,
                       const fs::path& outputApk,
                       const fs::path& androidJar,
                       const AndroidBuildTools& tools,
                       const fs::path& keystorePath,
                       const std::function<void(const std::string&)>& appendLog,
                       std::string& error) {
    std::error_code ec;
    fs::remove(outputApk, ec);

    const fs::path compiledResources = apkStageRoot / "compiled-res.zip";
    const fs::path unsignedApk = apkStageRoot / "unsigned.apk";
    const fs::path alignedApk = apkStageRoot / "aligned.apk";
    const fs::path generatedJava = apkStageRoot / "gen";
    fs::create_directories(generatedJava, ec);
    if (ec) {
        error = "Failed to create Android generated-source directory.";
        return false;
    }

    auto runStep = [&](const std::string& command, const std::string& failure) {
        int exitCode = 0;
        if (appendLog) appendLog("Running: " + command);
        if (!runCommandStreaming(command + " 2>&1", appendLog, &exitCode)) {
            error = failure + " (exit code " + std::to_string(exitCode) + ").";
            return false;
        }
        return true;
    };

    auto collectFilesWithExtension = [&](const fs::path& root, const std::string& extension) {
        std::vector<fs::path> files;
        std::error_code iterEc;
        if (!fs::is_directory(root, iterEc)) {
            return files;
        }
        for (auto it = fs::recursive_directory_iterator(root, iterEc);
             !iterEc && it != fs::recursive_directory_iterator();
             it.increment(iterEc)) {
            if (iterEc || !it->is_regular_file()) {
                continue;
            }
            if (it->path().extension() == extension) {
                files.push_back(it->path());
            }
        }
        return files;
    };

    if (!runStep(quotePath(tools.aapt2) + " compile --dir " +
                 quotePath(apkStageRoot / "res") + " -o " + quotePath(compiledResources),
                 "aapt2 resource compile failed")) {
        return false;
    }

    if (!runStep(quotePath(tools.aapt2) + " link -o " + quotePath(unsignedApk) +
                 " -I " + quotePath(androidJar) +
                 " --manifest " + quotePath(apkStageRoot / "AndroidManifest.xml") +
                 " --java " + quotePath(generatedJava) +
                 " -A " + quotePath(apkStageRoot / "assets") +
                 " " + quotePath(compiledResources),
                 "aapt2 link failed")) {
        return false;
    }

    const fs::path javaSourceRoot = apkStageRoot / "java";
    const std::vector<fs::path> javaFiles = collectFilesWithExtension(javaSourceRoot, ".java");
    if (!javaFiles.empty()) {
        fs::path javac = findJavaTool("javac");
        if (javac.empty()) {
            error = "javac was not found in PATH; needed to compile the Android activity bridge.";
            return false;
        }
        if (tools.d8.empty()) {
            error = "Android SDK build-tools missing d8; needed to dex the Android activity bridge.";
            return false;
        }

        const fs::path classesDir = apkStageRoot / "classes";
        const fs::path dexDir = apkStageRoot / "dex";
        fs::remove_all(classesDir, ec);
        fs::remove_all(dexDir, ec);
        fs::create_directories(classesDir, ec);
        fs::create_directories(dexDir, ec);
        if (ec) {
            error = "Failed to create Android Java compile directories.";
            return false;
        }

        std::string javacCmd =
            quotePath(javac) +
            " -encoding UTF-8 -source 8 -target 8 -Xlint:-options -bootclasspath " + quotePath(androidJar) +
            " -d " + quotePath(classesDir);
        for (const fs::path& javaFile : javaFiles) {
            javacCmd += " " + quotePath(javaFile);
        }
        if (!runStep(javacCmd, "javac failed for Android activity bridge")) {
            return false;
        }

        const std::vector<fs::path> classFiles = collectFilesWithExtension(classesDir, ".class");
        if (classFiles.empty()) {
            error = "Android activity bridge produced no .class files.";
            return false;
        }
        fs::path jarTool = findJavaTool("jar");
        if (jarTool.empty()) {
            error = "jar was not found in PATH; needed to package Android activity bridge classes for d8.";
            return false;
        }
        const fs::path classesJar = apkStageRoot / "classes.jar";
        fs::remove(classesJar, ec);
        const std::string jarCmd =
            quotePath(jarTool) + " cf " + quotePath(classesJar) +
            " -C " + quotePath(classesDir) + " .";
        if (!runStep(jarCmd, "jar failed for Android activity bridge")) {
            return false;
        }

        std::string d8Cmd =
            quotePath(tools.d8) +
            " --min-api 26 --output " + quotePath(dexDir) +
            " " + quotePath(classesJar);
        if (!runStep(d8Cmd, "d8 failed for Android activity bridge")) {
            return false;
        }

        fs::copy_file(dexDir / "classes.dex", apkStageRoot / "classes.dex",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Failed to stage Android activity bridge dex.";
            return false;
        }
    }

    fs::path jarTool = findJavaTool("jar");
    if (jarTool.empty()) {
        error = "The JDK jar tool was not found. Set JAVA_HOME or add the JDK bin directory to PATH.";
        return false;
    }
    std::string jarCommand = quotePath(jarTool) + " ufM " + quotePath(unsignedApk) +
        " -C " + quotePath(apkStageRoot) + " lib";
    if (fs::exists(apkStageRoot / "classes.dex", ec) && !ec) {
        jarCommand += " -C " + quotePath(apkStageRoot) + " classes.dex";
    }
    if (!runStep(jarCommand,
                 "Failed to add native libraries to APK")) {
        return false;
    }

    if (!runStep(quotePath(tools.zipalign) + " -P 16 -f 4 " +
                 quotePath(unsignedApk) + " " + quotePath(alignedApk),
                 "zipalign failed")) {
        return false;
    }

    if (!ensureAndroidDebugKeystore(keystorePath, appendLog, error)) {
        return false;
    }

    if (!runStep(quotePath(tools.apksigner) +
                 " sign --ks " + quotePath(keystorePath) +
                 " --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android" +
                 " --out " + quotePath(outputApk) + " " + quotePath(alignedApk),
                 "apksigner failed")) {
        return false;
    }

    return true;
}

fs::path resolveCurrentExecutablePath() {
#if defined(__linux__)
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe.empty()) {
        return exe.lexically_normal();
    }
#elif defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len > 0) {
        return fs::path(buffer).lexically_normal();
    }
#endif

    std::error_code cwdEc;
#if defined(_WIN32)
    fs::path fallback = fs::current_path(cwdEc) / "Modularity.exe";
#else
    fs::path fallback = fs::current_path(cwdEc) / "Modularity";
#endif
    return fallback.lexically_normal();
}

bool relaunchEditorWithBackend(Modularity::GraphicsBackend backend,
                               const fs::path& projectFile,
                               std::string& error,
                               bool inVR = false) {
    std::error_code ec;
    fs::path normalizedProject = projectFile;
    if (normalizedProject.empty()) {
        error = "Project file path is empty.";
        return false;
    }
    if (normalizedProject.is_relative()) {
        normalizedProject = fs::absolute(normalizedProject, ec);
        if (ec) {
            error = "Failed to resolve project file path.";
            return false;
        }
    }
    normalizedProject = normalizedProject.lexically_normal();

    fs::path autoStartPath = fs::current_path(ec) / "autostart.modu";
    if (ec) {
        error = "Failed to resolve autostart.modu path.";
        return false;
    }

    std::ofstream autostart(autoStartPath, std::ios::trunc);
    if (!autostart.is_open()) {
        error = "Failed to write autostart.modu.";
        return false;
    }
    autostart << "project=" << normalizedProject.string() << "\n";
    autostart << "mode=editor\n";
    autostart << "oneshot=1\n";
    autostart.close();

    fs::path executablePath = resolveCurrentExecutablePath();
    if (executablePath.empty()) {
        error = "Failed to resolve editor executable path.";
        return false;
    }
    if (!fs::exists(executablePath, ec) || ec) {
        error = "Editor executable not found: " + executablePath.string();
        return false;
    }

    const std::string backendValue =
        (backend == Modularity::GraphicsBackend::Vulkan) ? "vulkan" : "opengl";
    const std::string vrArgument = inVR ? " --vr-editor" : "";
    std::string command;
#if defined(_WIN32)
    command = "cmd /C \"set MODULARITY_RENDER_BACKEND=" + backendValue + " && start \"\" " +
              quotePath(executablePath) + " --project " + quotePath(normalizedProject) + vrArgument + "\"";
#else
    command = "MODULARITY_RENDER_BACKEND=" + backendValue + " " +
              quotePath(executablePath) + " --project " + quotePath(normalizedProject) + vrArgument +
              " >/dev/null 2>&1 &";
#endif

    if (std::system(command.c_str()) != 0) {
        error = "Failed to launch editor process.";
        return false;
    }
    return true;
}

void cleanExportOutput(const fs::path& exportRoot, const char* exeBaseName, std::string& error) {
    std::error_code ec;
#ifdef _WIN32
    fs::path exePath = exportRoot / (std::string(exeBaseName) + ".exe");
    fs::path importLibPath = exportRoot / (std::string(exeBaseName) + ".lib");
#else
    fs::path exePath = exportRoot / exeBaseName;
#endif
    if (fs::exists(exePath)) {
        fs::remove(exePath, ec);
        if (ec) {
            error = "Failed to remove existing executable.";
            return;
        }
    }

#ifdef _WIN32
    if (fs::exists(importLibPath)) {
        fs::remove(importLibPath, ec);
        if (ec) {
            error = "Failed to remove existing import library.";
            return;
        }
    }
#endif

    fs::path projectDir = exportRoot / "Project";
    if (fs::exists(projectDir)) {
        fs::remove_all(projectDir, ec);
        if (ec) {
            error = "Failed to remove existing project files.";
            return;
        }
    }

    fs::path resourcesDir = exportRoot / "Resources";
    if (fs::exists(resourcesDir)) {
        fs::remove_all(resourcesDir, ec);
        if (ec) {
            error = "Failed to remove existing resources.";
            return;
        }
    }

    fs::path packagesDir = exportRoot / "Packages";
    if (fs::exists(packagesDir)) {
        fs::remove_all(packagesDir, ec);
        if (ec) {
            error = "Failed to remove existing packages.";
            return;
        }
    }

    fs::path templatesDir = exportRoot / "Template-Projects";
    if (fs::exists(templatesDir)) {
        fs::remove_all(templatesDir, ec);
        if (ec) {
            error = "Failed to remove existing template projects.";
            return;
        }
    }

    fs::path autostart = exportRoot / "autostart.modu";
    if (fs::exists(autostart)) {
        fs::remove(autostart, ec);
        if (ec) {
            error = "Failed to remove existing autostart.modu.";
            return;
        }
    }

    fs::path runtimeBundle = exportRoot / "content.modbundle";
    if (fs::exists(runtimeBundle)) {
        fs::remove(runtimeBundle, ec);
        if (ec) {
            error = "Failed to remove existing runtime bundle.";
            return;
        }
    }

    fs::path runtimeStage = exportRoot / "_runtime_stage";
    if (fs::exists(runtimeStage)) {
        fs::remove_all(runtimeStage, ec);
        if (ec) {
            error = "Failed to remove existing runtime staging directory.";
            return;
        }
    }
}

void cleanEditorExecutable(const fs::path& buildRoot) {
    std::error_code ec;
#ifdef _WIN32
    fs::path editorExe = buildRoot / "Modularity.exe";
#else
    fs::path editorExe = buildRoot / "Modularity";
#endif
    if (fs::exists(editorExe)) {
        fs::remove(editorExe, ec);
    }

    ec.clear();
    const fs::path autoStartPath = buildRoot / "autostart.modu";
    if (fs::exists(autoStartPath, ec)) {
        fs::remove(autoStartPath, ec);
    }
}

std::string RuntimeHashHex(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

uint64_t RuntimePathHash(const std::string& value) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : value) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool RuntimePathInsideRoot(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    fs::path normalizedPath = fs::weakly_canonical(path, ec);
    if (ec) normalizedPath = fs::absolute(path, ec);
    if (ec) normalizedPath = path.lexically_normal();

    ec.clear();
    fs::path normalizedRoot = fs::weakly_canonical(root, ec);
    if (ec) normalizedRoot = fs::absolute(root, ec);
    if (ec) normalizedRoot = root.lexically_normal();

    auto pathIt = normalizedPath.begin();
    auto rootIt = normalizedRoot.begin();
    for (; rootIt != normalizedRoot.end(); ++rootIt, ++pathIt) {
        if (pathIt == normalizedPath.end() || *pathIt != *rootIt) {
            return false;
        }
    }
    return true;
}

fs::path ResolveRuntimeSourcePath(const std::string& value, const fs::path& projectRoot) {
    if (value.empty()) return {};

    std::error_code ec;
    fs::path input(value);
    if (input.is_absolute()) {
        fs::path absolute = fs::absolute(input, ec);
        if (ec) absolute = input;
        return absolute.lexically_normal();
    }

    fs::path candidate = projectRoot / input;
    fs::path absolute = fs::absolute(candidate, ec);
    if (ec) absolute = candidate;
    if (!fs::exists(absolute)) {
        fs::path cwdCandidate = fs::current_path(ec) / input;
        if (!ec) {
            fs::path cwdAbsolute = fs::absolute(cwdCandidate, ec);
            if (!ec && fs::exists(cwdAbsolute)) {
                return cwdAbsolute.lexically_normal();
            }
        }
    }
    return absolute.lexically_normal();
}

bool EnsureDirectoryForFile(const fs::path& filePath, std::string& error) {
    std::error_code ec;
    const fs::path parent = filePath.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            error = "Failed to create directory: " + parent.string();
            return false;
        }
    }
    return true;
}

bool CopyFileIntoRuntimeRoot(const fs::path& sourcePath,
                             const fs::path& runtimeRoot,
                             const fs::path& relativePath,
                             std::string& error) {
    if (sourcePath.empty() || relativePath.empty()) {
        error = "Runtime copy received an empty path.";
        return false;
    }

    const fs::path destination = runtimeRoot / relativePath;
    if (!EnsureDirectoryForFile(destination, error)) {
        return false;
    }

    std::error_code ec;
    fs::copy_file(sourcePath, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Failed to copy runtime file '" + sourcePath.string() +
                "' to '" + destination.string() + "': " + ec.message();
        return false;
    }
    return true;
}

struct RuntimeCopySkips {
    uint64_t files = 0;
    uint64_t bytes = 0;
};

// Files that are never runtime content but ride along when a whole folder is staged: Unity .meta
// files from store packs, Blender's .blend1/.blend2 save backups, downloads that never finished
// (.part, .crdownload, and the hidden ".xdp-*" temp files Chromium browsers leave while
// downloading), and OS folder metadata. Nothing in the engine or player reads any of them.
bool IsRuntimeExportJunk(const fs::path& path) {
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "thumbs.db" || name == "desktop.ini" || name == ".ds_store") return true;
    if (name.rfind(".xdp-", 0) == 0) return true;

    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = name.substr(dot);
    if (ext == ".meta" || ext == ".part" || ext == ".crdownload") return true;
    if (ext.size() > 6 && ext.rfind(".blend", 0) == 0 &&
        std::all_of(ext.begin() + 6, ext.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return true;
    }
    return false;
}

bool CopyDirectoryIntoRuntimeRoot(const fs::path& sourceDir,
                                  const fs::path& runtimeRoot,
                                  const fs::path& relativeDir,
                                  std::string& error,
                                  RuntimeCopySkips* skipped = nullptr) {
    if (sourceDir.empty() || relativeDir.empty()) {
        error = "Runtime directory copy received an empty path.";
        return false;
    }

    std::error_code ec;
    if (!fs::exists(sourceDir, ec)) {
        error = "Runtime directory source does not exist: " + sourceDir.string();
        return false;
    }
    const fs::path destination = runtimeRoot / relativeDir;
    fs::create_directories(destination, ec);
    if (ec) {
        error = "Failed to create runtime directory: " + destination.string();
        return false;
    }

    for (auto it = fs::recursive_directory_iterator(sourceDir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec) {
            error = "Failed while scanning runtime directory: " + sourceDir.string();
            return false;
        }
        const fs::path entryPath = it->path();
        const fs::path rel = fs::relative(entryPath, sourceDir, ec);
        if (ec) {
            error = "Failed to relativize runtime directory entry: " + entryPath.string();
            return false;
        }
        const fs::path targetPath = destination / rel;
        if (it->is_directory()) {
            fs::create_directories(targetPath, ec);
            if (ec) {
                error = "Failed to create runtime directory: " + targetPath.string();
                return false;
            }
            continue;
        }
        if (!it->is_regular_file()) continue;
        if (IsRuntimeExportJunk(entryPath)) {
            if (skipped) {
                ++skipped->files;
                std::error_code sizeEc;
                const auto size = fs::file_size(entryPath, sizeEc);
                if (!sizeEc) skipped->bytes += size;
            }
            continue;
        }
        if (!EnsureDirectoryForFile(targetPath, error)) {
            return false;
        }
        fs::copy_file(entryPath, targetPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Failed to copy runtime directory file: " + entryPath.string() +
                    " -> " + targetPath.string() + " (" + ec.message() + ")";
            return false;
        }
    }
    return true;
}

// Only what the player loads: resolveScriptBinary looks for <stem>.dll (or .so) and never enters
// the dot directories. The compiler leaves far more beside them - the shared precompiled header
// (~270 MB), per-script .obj and .pdb files, .cpp/.rsp/.buildsig inputs and import libraries -
// which rebuilding needs and running does not.
bool CopyCompiledScriptBinaries(const fs::path& sourceDir,
                                const fs::path& runtimeRoot,
                                const fs::path& relativeDir,
                                std::string& error,
                                RuntimeCopySkips* skipped) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(sourceDir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec) {
            error = "Failed while scanning compiled scripts: " + sourceDir.string();
            return false;
        }
        const fs::path entryPath = it->path();
        if (it->is_directory()) {
            if (entryPath.filename().string().rfind('.', 0) == 0) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file()) continue;

        std::string ext = entryPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".dll" && ext != ".so" && ext != ".dylib") {
            if (skipped) {
                ++skipped->files;
                std::error_code sizeEc;
                const auto size = fs::file_size(entryPath, sizeEc);
                if (!sizeEc) skipped->bytes += size;
            }
            continue;
        }

        const fs::path rel = fs::relative(entryPath, sourceDir, ec);
        if (ec) {
            error = "Failed to relativize compiled script: " + entryPath.string();
            return false;
        }
        if (!CopyFileIntoRuntimeRoot(entryPath, runtimeRoot, relativeDir / rel, error)) {
            return false;
        }
    }
    return true;
}

// Carry the notices for third-party shared libraries copied into a desktop export. The FFmpeg
// notice also records its exact upstream revision and build configuration.
bool WriteShippedLibraryLicenses(const fs::path& exportRoot,
                                 const fs::path& sourceRoot,
                                 const std::function<void(const std::string&)>& appendLog) {
    std::error_code ec;
    bool shipsFfmpeg = false;
    bool shipsDiscord = false;
    for (const auto& entry : fs::directory_iterator(exportRoot, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("avcodec", 0) == 0 || name.rfind("libavcodec", 0) == 0) {
            shipsFfmpeg = true;
        }
        if (name == "discord_partner_sdk.dll" || name == "libdiscord_partner_sdk.so" ||
            name == "libdiscord_partner_sdk.dylib") shipsDiscord = true;
    }
    if (!shipsFfmpeg && !shipsDiscord) return true;

    const fs::path licensesDir = exportRoot / "licenses";
    fs::create_directories(licensesDir, ec);
    if (ec) {
        appendLog("Warning: could not create the licenses folder in the export.");
        return true;
    }

    if (shipsFfmpeg) {
        const fs::path licenseSource = sourceRoot / "Resources" / "Licenses" / "FFmpeg-LICENSE.txt";
        if (fs::exists(licenseSource, ec)) {
            fs::copy_file(licenseSource, licensesDir / "FFmpeg-LICENSE.txt",
                          fs::copy_options::overwrite_existing, ec);
            if (ec) appendLog("Warning: could not copy FFmpeg-LICENSE.txt into the export.");
        } else {
            appendLog("Warning: Resources/Licenses/FFmpeg-LICENSE.txt is missing; the export ships "
                      "FFmpeg without its licence text.");
        }

        std::ofstream notice(licensesDir / "FFmpeg-NOTICE.txt", std::ios::trunc);
        if (notice.is_open()) {
            const std::string version = Modularity::AudioExport::FfmpegVersion();
            notice << "This program uses libraries from the FFmpeg project (libavcodec, libavformat,\n"
                      "libavutil, libswresample, libswscale) under the GNU Lesser General Public\n"
                      "License version 3 or later. The full licence is in FFmpeg-LICENSE.txt.\n\n";
            notice << "FFmpeg version: " << (version.empty() ? "unknown" : version) << "\n";
            const size_t revision = version.rfind("-g");
            if (revision != std::string::npos) {
                const size_t end = version.find('-', revision + 2);
                notice << "Source code:    https://github.com/FFmpeg/FFmpeg/commit/"
                       << version.substr(revision + 2, end == std::string::npos ? end : end - revision - 2)
                       << "\n";
            }
            notice << "Also available from https://ffmpeg.org/download.html\n\n";
            notice << "The libraries are unmodified upstream builds, configured with:\n"
                   << Modularity::AudioExport::FfmpegConfiguration() << "\n\n";
            notice << "They ship as separate files beside the executable and may be replaced with\n"
                      "compatible builds of the same versions.\n";
        }
        appendLog("Wrote FFmpeg LGPL licence and notice to the export's licenses folder.");
    }

    if (shipsDiscord) {
        const fs::path noticeSource = sourceRoot / "src" / "ThirdParty" /
                                      "DiscordSocialSdk" / "License-Notices.txt";
        const fs::path discordLicenseDir = licensesDir / "DiscordSocialSdk";
        bool copiedNotice = false;
        fs::create_directories(discordLicenseDir, ec);
        if (!ec && fs::exists(noticeSource, ec)) {
            fs::copy_file(noticeSource, discordLicenseDir / "License-Notices.txt",
                          fs::copy_options::overwrite_existing, ec);
            copiedNotice = !ec;
        }
        if (!copiedNotice) {
            appendLog("Warning: could not copy Discord Social SDK license notices into the export.");
        } else {
            appendLog("Copied Discord Social SDK license notices into the export.");
        }
    }
    return true;
}

// Encoded clips are cached under the project's Library, which exports never ship.
void CompressStagedAudioForExport(const fs::path& runtimeStageRoot,
                                  const fs::path& projectRoot,
                                  float quality,
                                  const std::function<void(const std::string&)>& appendLog,
                                  const std::function<bool()>& cancelled) {
    if (!Modularity::AudioExport::EncoderAvailable()) {
        appendLog("Warning: audio left uncompressed - this editor build has no Vorbis encoder (FFmpeg libvorbis).");
        return;
    }
    const auto stats = Modularity::AudioExport::CompressStagedAudio(
        runtimeStageRoot, projectRoot / "Library" / "ExportCache" / "Audio", quality, appendLog, cancelled);
    if (stats.converted == 0 && stats.failed == 0 && stats.keptOriginal == 0) return;

    char line[256];
    std::snprintf(line, sizeof(line),
                  "Compressed %d audio clips to Vorbis (quality %.1f): %.1f MB -> %.1f MB "
                  "(%d reused from cache, %d kept as-is because Vorbis was not smaller, %d failed)",
                  stats.converted, quality,
                  static_cast<double>(stats.bytesBefore) / (1024.0 * 1024.0),
                  static_cast<double>(stats.bytesAfter) / (1024.0 * 1024.0),
                  stats.reusedFromCache, stats.keptOriginal, stats.failed);
    appendLog(line);
}

bool FilesHaveSameContents(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const auto sizeA = fs::file_size(a, ec);
    if (ec) return false;
    const auto sizeB = fs::file_size(b, ec);
    if (ec || sizeA != sizeB) return false;

    std::ifstream inA(a, std::ios::binary);
    std::ifstream inB(b, std::ios::binary);
    if (!inA.is_open() || !inB.is_open()) return false;
    std::vector<char> bufA(1 << 20);
    std::vector<char> bufB(1 << 20);
    while (inA && inB) {
        inA.read(bufA.data(), static_cast<std::streamsize>(bufA.size()));
        inB.read(bufB.data(), static_cast<std::streamsize>(bufB.size()));
        const std::streamsize countA = inA.gcount();
        if (countA != inB.gcount()) return false;
        if (std::memcmp(bufA.data(), bufB.data(), static_cast<size_t>(countA)) != 0) return false;
    }
    return true;
}

class RuntimeExportStager {
public:
    RuntimeExportStager(fs::path projectRootPath, fs::path runtimeRootPath)
        : projectRoot(std::move(projectRootPath)),
          runtimeRoot(std::move(runtimeRootPath)) {}

    // Labels the reference being staged, for the missing-asset report: the owner is e.g.
    // "Scene 'Main' > 'Tree (3)'" and the label "albedo texture".
    void setReferenceOwner(std::string owner) { referenceOwner = std::move(owner); }
    void setReferenceLabel(std::string label) { referenceLabel = std::move(label); }

    // Missing path -> every place that referenced it, in first-seen order of owners.
    const std::map<std::string, std::vector<std::string>>& missingReferences() const {
        return missingRefs;
    }

    // Junk files left out of staged asset folders (see IsRuntimeExportJunk).
    const RuntimeCopySkips& skippedJunk() const { return junkSkipped; }

    bool stageFileReference(std::string& value,
                            const std::string& externalCategory,
                            std::string& error) {
        if (value.empty()) return true;
        std::error_code ec;
        const fs::path sourcePath = resolveSource(value);
        if (sourcePath.empty()) {
            recordMissing(value);
            return true;
        }

        const std::string sourceKey = sourcePath.lexically_normal().string();
        auto cacheIt = stagedFileRefs.find(sourceKey);
        if (cacheIt != stagedFileRefs.end()) {
            value = cacheIt->second;
            return true;
        }

        fs::path relativePath;
        if (RuntimePathInsideRoot(sourcePath, projectRoot)) {
            relativePath = fs::relative(sourcePath, projectRoot, ec);
            if (ec || relativePath.empty()) {
                error = "Failed to resolve runtime asset path relative to project: " + sourcePath.string();
                return false;
            }
        } else {
            const std::string hashedName = sourcePath.stem().string() + "_" +
                                           RuntimeHashHex(RuntimePathHash(sourceKey)) +
                                           sourcePath.extension().string();
            relativePath = fs::path("Assets") / "RuntimeImported" / externalCategory / hashedName;
        }

        if (!CopyFileIntoRuntimeRoot(sourcePath, runtimeRoot, relativePath, error)) {
            return false;
        }

        const std::string remapped = relativePath.generic_string();
        stagedFileRefs[sourceKey] = remapped;
        value = remapped;
        return true;
    }

    bool stageDirectoryBackedReference(std::string& value,
                                       const std::string& externalCategory,
                                       std::string& error) {
        if (value.empty()) return true;
        std::error_code ec;
        const fs::path sourcePath = resolveSource(value);
        if (sourcePath.empty()) {
            recordMissing(value);
            return true;
        }

        const fs::path sourceDir = sourcePath.parent_path();
        const std::string sourceKey = sourceDir.lexically_normal().string();
        auto dirIt = stagedDirectoryRoots.find(sourceKey);
        fs::path relativeDir;
        if (dirIt != stagedDirectoryRoots.end()) {
            relativeDir = dirIt->second;
        } else if (RuntimePathInsideRoot(sourceDir, projectRoot)) {
            relativeDir = fs::relative(sourceDir, projectRoot, ec);
            if (ec || relativeDir.empty()) {
                error = "Failed to resolve runtime directory relative path: " + sourceDir.string();
                return false;
            }
            if (!CopyDirectoryIntoRuntimeRoot(sourceDir, runtimeRoot, relativeDir, error, &junkSkipped)) {
                return false;
            }
            stagedDirectoryRoots[sourceKey] = relativeDir;
        } else {
            const std::string dirName = sourceDir.filename().empty()
                ? "dir"
                : sourceDir.filename().string();
            relativeDir = fs::path("Assets") / "RuntimeImported" / externalCategory /
                          (dirName + "_" + RuntimeHashHex(RuntimePathHash(sourceKey)));
            if (!CopyDirectoryIntoRuntimeRoot(sourceDir, runtimeRoot, relativeDir, error, &junkSkipped)) {
                return false;
            }
            stagedDirectoryRoots[sourceKey] = relativeDir;
        }

        value = (relativeDir / sourcePath.filename()).generic_string();
        return true;
    }

private:
    // The project-relative lookup first, then the editor's foreign-project rescue, so anything the
    // editor can load also ships. Empty when neither finds a file.
    fs::path resolveSource(const std::string& value) const {
        std::error_code ec;
        const fs::path direct = ResolveRuntimeSourcePath(value, projectRoot);
        if (!direct.empty() && fs::exists(direct, ec) && !ec) return direct;
        ec.clear();
        const fs::path rescued(Modularity::Platform::ResolveAssetPath(value));
        if (fs::exists(rescued, ec) && !ec) {
            fs::path absolute = fs::absolute(rescued, ec);
            return (ec ? rescued : absolute).lexically_normal();
        }
        return {};
    }

    // Unity-style missing reference: the build keeps the object but drops what it pointed at,
    // and the export reports it instead of failing on the first broken path.
    void recordMissing(std::string& value) {
        std::string where = referenceOwner.empty() ? std::string("unknown owner") : referenceOwner;
        if (!referenceLabel.empty()) where += " " + referenceLabel;
        missingRefs[value].push_back(std::move(where));
        value.clear();
    }

    fs::path projectRoot;
    fs::path runtimeRoot;
    std::unordered_map<std::string, std::string> stagedFileRefs;
    std::unordered_map<std::string, fs::path> stagedDirectoryRoots;
    std::string referenceOwner;
    std::string referenceLabel;
    std::map<std::string, std::vector<std::string>> missingRefs;
    RuntimeCopySkips junkSkipped;
};

// One log line per missing path, naming a few of the objects that used it.
std::vector<std::string> DescribeMissingRuntimeAssets(const RuntimeExportStager& stager) {
    std::vector<std::string> lines;
    for (const auto& [path, owners] : stager.missingReferences()) {
        std::string line = "Missing asset '" + path + "' was left out of the build; used by ";
        const size_t shown = std::min<size_t>(owners.size(), 3);
        for (size_t i = 0; i < shown; ++i) {
            if (i > 0) line += ", ";
            line += owners[i];
        }
        if (owners.size() > shown) {
            line += " (+" + std::to_string(owners.size() - shown) + " more)";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

bool RemapSceneObjectForRuntime(SceneObject& obj,
                                RuntimeExportStager& stager,
                                std::string& error) {
    return ForEachSceneObjectAssetReference(obj,
        [&](std::string& value, SceneAssetRefKind kind, const char* category, const char* label) {
            stager.setReferenceLabel(label);
            return kind == SceneAssetRefKind::DirectoryBacked
                ? stager.stageDirectoryBackedReference(value, category, error)
                : stager.stageFileReference(value, category, error);
        });
}

bool RemapSkyboxSettingsForRuntime(SkyboxSettings& settings,
                                   RuntimeExportStager& stager,
                                   std::string& error) {
    stager.setReferenceLabel("sun texture");
    if (!stager.stageFileReference(settings.sunTexturePath, "Textures", error)) return false;
    stager.setReferenceLabel("moon texture");
    if (!stager.stageFileReference(settings.moonTexturePath, "Textures", error)) return false;
    stager.setReferenceLabel("scrolling texture");
    if (!stager.stageFileReference(settings.scrollingTexturePath, "Textures", error)) return false;
    return true;
}

// The vertex/fragment pair each staged object draws with, as the staged scene spells it, so
// the player can look them up in its shader cache under the exact same key.
using RuntimeShaderPairs = std::set<std::pair<std::string, std::string>>;

bool StageRuntimeScene(const fs::path& sourceScenePath,
                       const std::string& sceneName,
                       RuntimeExportStager& stager,
                       const fs::path& runtimeRoot,
                       std::string& error,
                       RuntimeShaderPairs* shaderPairs = nullptr) {
    std::vector<SceneObject> sceneObjects;
    int nextId = 0;
    int sceneVersion = 20;
    float timeOfDay = -1.0f;
    SkyboxSettings skyboxSettings;
    if (!SceneSerializer::loadSceneDeferred(sourceScenePath, sceneObjects, nextId, sceneVersion, &timeOfDay, &skyboxSettings)) {
        error = "Failed to load scene for runtime export: " + sourceScenePath.string();
        return false;
    }

    for (SceneObject& obj : sceneObjects) {
        stager.setReferenceOwner("Scene '" + sceneName + "' > '" + obj.name + "'");
        if (!RemapSceneObjectForRuntime(obj, stager, error)) {
            error = "Scene '" + sceneName + "': " + error;
            return false;
        }
        if (shaderPairs && obj.hasRenderer && obj.renderType != RenderType::None) {
            shaderPairs->emplace(obj.vertexShaderPath, obj.fragmentShaderPath);
        }
    }
    stager.setReferenceOwner("Scene '" + sceneName + "' skybox");
    if (!RemapSkyboxSettingsForRuntime(skyboxSettings, stager, error)) {
        error = "Scene '" + sceneName + "': " + error;
        return false;
    }

    const fs::path runtimeScenePath = runtimeRoot / "Assets" / "Scenes" / (sceneName + ".scene");
    if (!EnsureDirectoryForFile(runtimeScenePath, error)) {
        return false;
    }
    if (!SceneSerializer::saveScene(runtimeScenePath,
                                    sceneObjects,
                                    nextId,
                                    timeOfDay < 0.0f ? 0.5f : timeOfDay,
                                    skyboxSettings)) {
        error = "Failed to save staged runtime scene: " + runtimeScenePath.string();
        return false;
    }
    return true;
}

// Writes what the player needs to have its shaders ready before the first scene draws:
//  - Resources/Shaders/preload.list, one "vert|frag" pair per line (empty = the default
//    material shader), which Engine::preloadRuntimeShaders compiles at boot;
//  - for GLES 2, a <name>.es2.glsl sibling for every stage those pairs use, plus the skybox.
// A shader that will not convert is a warning, not a failed export: the rest of the game
// still runs, and the log says which object's look is affected.
bool StageRuntimeShaderPrograms(const fs::path& runtimeRoot,
                                const RuntimeShaderPairs& pairs,
                                bool gles2Shaders,
                                bool preloadShaders,
                                const std::function<void(const std::string&)>& log,
                                std::string& error) {
    if (!gles2Shaders && !preloadShaders) return true;

    const fs::path listPath = runtimeRoot / "Resources" / "Shaders" / "preload.list";
    if (!EnsureDirectoryForFile(listPath, error)) return false;
    {
        std::ofstream out(listPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            error = "Failed to write " + listPath.string();
            return false;
        }
        for (const auto& [vert, frag] : pairs) {
            out << vert << "|" << frag << "\n";
        }
    }
    log("Shader preload: " + std::to_string(pairs.size()) + " program(s) listed for boot.");
    if (!gles2Shaders) return true;

    const std::string defaultVert = "Resources/Shaders/vert.glsl";
    const std::string defaultFrag = "Resources/Shaders/frag.glsl";
    std::set<std::pair<std::string, Modularity::Gles2ShaderStage>> stages;
    for (const auto& [vert, frag] : pairs) {
        stages.emplace(vert.empty() ? defaultVert : vert, Modularity::Gles2ShaderStage::Vertex);
        stages.emplace(frag.empty() ? defaultFrag : frag, Modularity::Gles2ShaderStage::Fragment);
    }
    stages.emplace("Resources/Shaders/skybox_vert.glsl", Modularity::Gles2ShaderStage::Vertex);
    stages.emplace("Resources/Shaders/skybox_frag.glsl", Modularity::Gles2ShaderStage::Fragment);

    int converted = 0;
    int failed = 0;
    for (const auto& [shaderPath, stage] : stages) {
        fs::path staged(shaderPath);
        if (staged.is_relative()) staged = runtimeRoot / staged;
        std::ifstream in(staged, std::ios::binary);
        if (!in.is_open()) {
            log("Warning: GLES 2.0: shader '" + shaderPath + "' is not in the build, skipped.");
            ++failed;
            continue;
        }
        std::stringstream source;
        source << in.rdbuf();
        const Modularity::Gles2TranspileResult result =
            Modularity::TranspileShaderToGles2(source.str(), stage);
        if (!result.success) {
            log("Warning: GLES 2.0: '" + shaderPath + "' was not converted: " + result.error);
            ++failed;
            continue;
        }
        const fs::path es2Path(Modularity::Gles2ShaderSiblingPath(staged.string()));
        std::ofstream out(es2Path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            error = "Failed to write " + es2Path.string();
            return false;
        }
        out << result.source;
        ++converted;
    }
    log("GLES 2.0 shaders: " + std::to_string(converted) + " converted" +
        (failed > 0 ? ", " + std::to_string(failed) + " skipped (see warnings above)" : std::string()) + ".");
    return true;
}

bool CopyRuntimeResourcesSubset(const fs::path& sourceRoot,
                                const fs::path& runtimeRoot,
                                bool leanRuntimeExport,
                                std::string& error) {
    const fs::path resourcesRoot = sourceRoot / "Resources";
    const fs::path docsRoot = sourceRoot / "docs";
    if (!CopyDirectoryIntoRuntimeRoot(resourcesRoot / "Shaders", runtimeRoot, fs::path("Resources") / "Shaders", error)) {
        return false;
    }
    if (!CopyDirectoryIntoRuntimeRoot(resourcesRoot / "Textures", runtimeRoot, fs::path("Resources") / "Textures", error)) {
        return false;
    }
    if (fs::exists(resourcesRoot / "Fonts")) {
        if (!CopyDirectoryIntoRuntimeRoot(resourcesRoot / "Fonts", runtimeRoot, fs::path("Resources") / "Fonts", error)) {
            return false;
        }
    }

    if (fs::exists(resourcesRoot / "Engine-Root")) {
        if (leanRuntimeExport) {
            if (fs::exists(resourcesRoot / "Engine-Root" / "Modu-Logo.png")) {
                if (!CopyFileIntoRuntimeRoot(resourcesRoot / "Engine-Root" / "Modu-Logo.png",
                                             runtimeRoot,
                                             fs::path("Resources") / "Engine-Root" / "Modu-Logo.png",
                                             error)) {
                    return false;
                }
            }
            if (fs::exists(resourcesRoot / "Engine-Root" / "Skybox")) {
                if (!CopyDirectoryIntoRuntimeRoot(resourcesRoot / "Engine-Root" / "Skybox",
                                                  runtimeRoot,
                                                  fs::path("Resources") / "Engine-Root" / "Skybox",
                                                  error)) {
                    return false;
                }
            }
            if (fs::exists(resourcesRoot / "Engine-Root" / "Textures")) {
                if (!CopyDirectoryIntoRuntimeRoot(resourcesRoot / "Engine-Root" / "Textures",
                                                  runtimeRoot,
                                                  fs::path("Resources") / "Engine-Root" / "Textures",
                                                  error)) {
                    return false;
                }
            }
        } else {
            if (!CopyDirectoryIntoRuntimeRoot(resourcesRoot / "Engine-Root",
                                              runtimeRoot,
                                              fs::path("Resources") / "Engine-Root",
                                              error)) {
                return false;
            }
        }
    }
    if (!leanRuntimeExport && fs::exists(docsRoot)) {
        if (!CopyDirectoryIntoRuntimeRoot(docsRoot, runtimeRoot, "docs", error)) {
            return false;
        }
    }
    return true;
}

std::vector<RuntimeBundleEntry> CollectRuntimeBundleEntries(const fs::path& runtimeRoot) {
    std::vector<RuntimeBundleEntry> entries;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(runtimeRoot, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec || !it->is_regular_file()) continue;
        const fs::path sourcePath = it->path();
        const fs::path archivePath = fs::relative(sourcePath, runtimeRoot, ec);
        if (ec || archivePath.empty()) continue;
        entries.push_back({sourcePath, archivePath});
    }
    std::sort(entries.begin(), entries.end(),
              [](const RuntimeBundleEntry& a, const RuntimeBundleEntry& b) {
                  return a.archivePath.generic_string() < b.archivePath.generic_string();
              });
    return entries;
}

std::string BuildSizeCategoryForPath(const fs::path& archivePath) {
    std::string p = archivePath.generic_string();
    std::string lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string ext = archivePath.extension().string();
    std::string lowerExt = ext;
    std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower.find("docs/") == 0 ||
        lower.find("template-projects/") == 0 ||
        lower.find("resources/scriptsdk/") == 0 ||
        lower.find("resources/engine-root/ai") == 0 ||
        lower.find("editor") != std::string::npos) {
        return "editor-only";
    }
    if (lowerExt == ".pdb" || lowerExt == ".ilk" || lowerExt == ".exp" ||
        lowerExt == ".map" || lowerExt == ".debug" || lower.find(".dsym/") != std::string::npos) {
        return "debug-symbols";
    }
    if (lower.find("packages/") == 0 ||
        lower.find("library/compiledscripts/") == 0 ||
        lower.find("library/installedpackages/") == 0 ||
        lower.find("scripts/managed/bin/") == 0) {
        return "modules";
    }
    if (lower.find("resources/") == 0) {
        return "engine-resources";
    }
    if (lower.find("assets/") == 0) {
        return "assets";
    }
    if (lowerExt == ".so" || lowerExt == ".dll" || lowerExt == ".dylib" ||
        lower.find("modularityplayer") != std::string::npos) {
        return "runtime";
    }
    return "other";
}

bool WriteBuildSizeReports(const fs::path& exportRoot,
                           const std::vector<RuntimeBundleEntry>& bundleEntries,
                           const std::vector<fs::path>& looseFiles,
                           const fs::path& archivePath,
                           const fs::path& projectRoot,
                           std::string& error) {
    struct SizeItem {
        fs::path sourcePath;
        fs::path displayPath;
        std::string category;
        uint64_t bytes = 0;
    };
    std::vector<SizeItem> items;
    std::map<std::string, uint64_t> totals;
    uint64_t totalBytes = 0;
    std::error_code ec;

    auto addItem = [&](const fs::path& source, const fs::path& display, const std::string& category) {
        if (source.empty() || !fs::exists(source, ec) || ec || !fs::is_regular_file(source, ec)) {
            ec.clear();
            return;
        }
        uint64_t bytes = static_cast<uint64_t>(fs::file_size(source, ec));
        if (ec) {
            ec.clear();
            return;
        }
        items.push_back({source, display, category, bytes});
        totals[category] += bytes;
        totalBytes += bytes;
    };

    for (const RuntimeBundleEntry& entry : bundleEntries) {
        addItem(entry.sourcePath, entry.archivePath, BuildSizeCategoryForPath(entry.archivePath));
    }
    for (const fs::path& loose : looseFiles) {
        std::error_code relEc;
        fs::path rel = fs::relative(loose, exportRoot, relEc);
        addItem(loose, relEc ? loose.filename() : rel, BuildSizeCategoryForPath(relEc ? loose.filename() : rel));
    }
    if (!archivePath.empty()) {
        addItem(archivePath, archivePath.filename(), "archive");
    }

    std::sort(items.begin(), items.end(), [](const SizeItem& a, const SizeItem& b) {
        return a.bytes > b.bytes;
    });

    std::vector<std::string> warnings;
    for (const SizeItem& item : items) {
        if (item.category == "debug-symbols") {
            warnings.push_back("Debug/build artifact in export: " + item.displayPath.generic_string());
        } else if (item.category == "editor-only") {
            warnings.push_back("Editor/development file in export: " + item.displayPath.generic_string());
        } else {
            std::string ext = item.displayPath.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".a" || ext == ".lib") {
                warnings.push_back("Static/import library in runtime payload: " + item.displayPath.generic_string());
            }
        }
        if (warnings.size() >= 40) break;
    }

    std::vector<fs::path> unusedCandidates;
    fs::path assetsRoot = projectRoot / "Assets";
    if (fs::exists(assetsRoot)) {
        std::unordered_set<std::string> stagedAssetNames;
        for (const RuntimeBundleEntry& entry : bundleEntries) {
            if (entry.archivePath.generic_string().find("Assets/") == 0) {
                stagedAssetNames.insert(entry.archivePath.filename().string());
            }
        }
        for (auto it = fs::recursive_directory_iterator(assetsRoot, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (ec) break;
            if (!it->is_regular_file()) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".scene" || ext == ".moducpp" || ext == ".mko" || ext == ".modumako" ||
                ext == ".cpp" || ext == ".h") continue;
            if (stagedAssetNames.find(it->path().filename().string()) == stagedAssetNames.end()) {
                unusedCandidates.push_back(it->path());
                if (unusedCandidates.size() >= 40) break;
            }
        }
    }

    fs::path mdPath = exportRoot / "BuildSizeReport.md";
    fs::create_directories(exportRoot, ec);
    std::ofstream md(mdPath, std::ios::trunc);
    if (!md.is_open()) {
        error = "Failed to write build size report: " + mdPath.string();
        return false;
    }
    md << "# Build Size Report\n\n";
    md << "Total scanned size: " << totalBytes << " bytes\n\n";
    md << "## Categories\n\n";
    for (const auto& [category, bytes] : totals) {
        md << "- " << category << ": " << bytes << " bytes\n";
    }
    md << "\n## Largest Files\n\n";
    const size_t topCount = std::min<size_t>(20, items.size());
    for (size_t i = 0; i < topCount; ++i) {
        md << "- " << items[i].displayPath.generic_string()
           << " (" << items[i].category << ", " << items[i].bytes << " bytes)\n";
    }
    if (!warnings.empty()) {
        md << "\n## Warnings\n\n";
        for (const std::string& warning : warnings) md << "- " << warning << "\n";
    }
    if (!unusedCandidates.empty()) {
        md << "\n## Possible Unused Assets\n\n";
        for (const fs::path& path : unusedCandidates) {
            std::error_code relEc;
            fs::path rel = fs::relative(path, projectRoot, relEc);
            md << "- " << (relEc ? path.generic_string() : rel.generic_string()) << "\n";
        }
    }

    fs::path jsonPath = exportRoot / "BuildSizeReport.json";
    std::ofstream json(jsonPath, std::ios::trunc);
    if (!json.is_open()) {
        error = "Failed to write build size JSON report: " + jsonPath.string();
        return false;
    }
    auto jsonEscape = [](const std::string& value) {
        std::string out;
        for (char c : value) {
            if (c == '"' || c == '\\') out.push_back('\\');
            if (c == '\n') out += "\\n";
            else out.push_back(c);
        }
        return out;
    };
    json << "{\n  \"totalBytes\": " << totalBytes << ",\n  \"categories\": {";
    bool first = true;
    for (const auto& [category, bytes] : totals) {
        json << (first ? "\n" : ",\n") << "    \"" << jsonEscape(category) << "\": " << bytes;
        first = false;
    }
    json << "\n  },\n  \"largestFiles\": [\n";
    for (size_t i = 0; i < topCount; ++i) {
        json << "    {\"path\": \"" << jsonEscape(items[i].displayPath.generic_string())
             << "\", \"category\": \"" << jsonEscape(items[i].category)
             << "\", \"bytes\": " << items[i].bytes << "}";
        if (i + 1 < topCount) json << ",";
        json << "\n";
    }
    json << "  ],\n  \"warnings\": [\n";
    for (size_t i = 0; i < warnings.size(); ++i) {
        json << "    \"" << jsonEscape(warnings[i]) << "\"";
        if (i + 1 < warnings.size()) json << ",";
        json << "\n";
    }
    json << "  ]\n}\n";
    return true;
}

fs::path BuildRuntimeCacheRoot(const fs::path& appDataPath,
                               const fs::path& bundlePath) {
    std::error_code ec;
    const uint64_t fileSize = fs::exists(bundlePath, ec) ? fs::file_size(bundlePath, ec) : 0ull;
    ec.clear();
    const auto writeTime = fs::exists(bundlePath, ec) ? fs::last_write_time(bundlePath, ec) : fs::file_time_type{};
    const auto timeTicks = static_cast<uint64_t>(writeTime.time_since_epoch().count());
    const std::string seed = bundlePath.lexically_normal().string() + "|" +
                             std::to_string(fileSize) + "|" +
                             std::to_string(timeTicks);
    const std::string cacheId = RuntimeHashHex(RuntimePathHash(seed));
    return appDataPath / "RuntimeCache" / cacheId;
}

bool RuntimeCacheEntryReady(const fs::path& cacheRoot) {
    std::error_code ec;
    if (!fs::exists(cacheRoot / kRuntimeCacheReadyMarker, ec) || ec) {
        return false;
    }
    ec.clear();
    return fs::exists(cacheRoot / "project.modu", ec) && !ec;
}

void PruneRuntimeCache(const fs::path& appDataPath,
                       const fs::path& activeCacheRoot) {
    const fs::path runtimeCacheRoot = appDataPath / "RuntimeCache";
    std::error_code ec;
    if (!fs::exists(runtimeCacheRoot, ec) || ec) {
        return;
    }

    struct CacheEntry {
        fs::path path;
        fs::file_time_type time;
        bool keep = false;
    };

    std::vector<CacheEntry> validEntries;
    const auto now = fs::file_time_type::clock::now();

    for (auto it = fs::directory_iterator(runtimeCacheRoot, ec);
         !ec && it != fs::directory_iterator(); ++it) {
        if (!it->is_directory()) {
            continue;
        }

        const fs::path entryPath = it->path();
        if (entryPath == activeCacheRoot) {
            std::error_code activeEc;
            validEntries.push_back({entryPath, fs::last_write_time(entryPath, activeEc), true});
            continue;
        }

        if (!RuntimeCacheEntryReady(entryPath)) {
            std::error_code removeEc;
            fs::remove_all(entryPath, removeEc);
            continue;
        }

        std::error_code timeEc;
        const fs::file_time_type writeTime = fs::last_write_time(entryPath, timeEc);
        if (timeEc) {
            std::error_code removeEc;
            fs::remove_all(entryPath, removeEc);
            continue;
        }

        validEntries.push_back({entryPath, writeTime, false});
    }

    std::sort(validEntries.begin(), validEntries.end(),
              [](const CacheEntry& a, const CacheEntry& b) {
                  return a.time > b.time;
              });

    size_t keptEntries = 0;
    for (const CacheEntry& entry : validEntries) {
        const bool expired = !entry.keep && (now - entry.time) > kRuntimeCacheMaxAge;
        const bool overLimit = !entry.keep && keptEntries >= kRuntimeCacheMaxEntries;
        if (expired || overLimit) {
            std::error_code removeEc;
            fs::remove_all(entry.path, removeEc);
            continue;
        }
        ++keptEntries;
    }
}
} // namespace
#pragma endregion

#pragma region Window + Selection Utilities
fs::path Engine::resolveProjectAssetPath(const std::string& rawPath) const {
    if (rawPath.empty()) {
        return {};
    }

    std::error_code ec;
    fs::path candidate(rawPath);
    if (fs::exists(candidate, ec) && !ec) {
        return candidate.lexically_normal();
    }

    if (projectManager.currentProject.isLoaded && !projectManager.currentProject.projectPath.empty()) {
        ec.clear();
        fs::path projectCandidate = projectManager.currentProject.projectPath / candidate;
        if (fs::exists(projectCandidate, ec) && !ec) {
            return projectCandidate.lexically_normal();
        }
    }

    return candidate.lexically_normal();
}

fs::path Engine::resolveProjectPathFromScript(const std::string& rawPath) const {
    if (rawPath.empty()) {
        return {};
    }

    auto expandScriptRootToken = [&](const std::string& value) -> fs::path {
        struct TokenRoot {
            const char* token;
            fs::path root;
        };

        const std::array<TokenRoot, 3> tokenRoots = {{
            {"MODU_ROOT", getProgramRootPathFromScript()},
            {"PROJECT_ROOT", projectManager.currentProject.projectPath},
            {"DOCS_ROOT", getEngineDocsRootPathFromScript()}
        }};

        for (const TokenRoot& entry : tokenRoots) {
            const std::string token = entry.token;
            if (value == token) {
                return entry.root;
            }
            if (value.size() > token.size() &&
                value.compare(0, token.size(), token) == 0 &&
                (value[token.size()] == '/' || value[token.size()] == '\\')) {
                const std::string suffix = value.substr(token.size() + 1);
                if (entry.root.empty()) {
                    return {};
                }
                return (entry.root / fs::path(suffix)).lexically_normal();
            }
        }

        return {};
    };

    if (fs::path expanded = expandScriptRootToken(rawPath); !expanded.empty()) {
        return expanded;
    }

    fs::path candidate(rawPath);
    if (candidate.is_absolute()) {
        return candidate.lexically_normal();
    }

    if (projectManager.currentProject.isLoaded && !projectManager.currentProject.projectPath.empty()) {
        return (projectManager.currentProject.projectPath / candidate).lexically_normal();
    }

    std::error_code ec;
    return (fs::current_path(ec) / candidate).lexically_normal();
}

fs::path Engine::getProgramRootPathFromScript() const {
    fs::path executablePath = resolveCurrentExecutablePath();
    if (!executablePath.empty()) {
        return executablePath.parent_path().lexically_normal();
    }

    std::error_code ec;
    return fs::current_path(ec).lexically_normal();
}

fs::path Engine::getPersistentDataPathFromScript(const std::string& subFolder) const {
    fs::path root = projectManager.appDataPath;
    if (root.empty()) {
        root = getProgramRootPathFromScript();
    }
    if (root.empty()) {
        return {};
    }

    root /= "GameData";

    std::string project = projectManager.currentProject.isLoaded
                              ? projectManager.currentProject.name
                              : std::string();
    std::string sanitized;
    sanitized.reserve(project.size());
    for (char c : project) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == ' ') {
            sanitized.push_back(c);
        } else {
            sanitized.push_back('_');
        }
    }
    while (!sanitized.empty() && (sanitized.back() == ' ' || sanitized.back() == '.')) {
        sanitized.pop_back();
    }
    root /= sanitized.empty() ? std::string("Untitled Project") : sanitized;

    if (!subFolder.empty()) {
        const fs::path relative = fs::path(subFolder).lexically_normal();
        if (!relative.is_absolute() && relative.string().find("..") == std::string::npos) {
            root /= relative;
        }
    }

    std::error_code ec;
    fs::create_directories(root, ec);
    return root.lexically_normal();
}

fs::path Engine::getEngineDocsRootPathFromScript() const {
    std::vector<fs::path> roots;
    roots.push_back(getProgramRootPathFromScript());
    std::error_code ec;
    roots.push_back(fs::current_path(ec));

    for (const fs::path& root : roots) {
        fs::path candidate = root;
        for (int depth = 0; depth < 5 && !candidate.empty(); ++depth) {
            fs::path docsPath = candidate / "docs";
            if (fs::exists(docsPath, ec) && fs::is_directory(docsPath, ec) && !ec) {
                return docsPath.lexically_normal();
            }
            ec.clear();
            fs::path parent = candidate.parent_path();
            if (parent == candidate) {
                break;
            }
            candidate = parent;
        }
    }

    return {};
}

void window_size_callback(GLFWwindow* window, int width, int height) {
    if (auto* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window))) {
        engine->onWindowResized(width, height);
    }
    if (glfwGetCurrentContext() != nullptr) {
        glViewport(0, 0, width, height);
    }
}

SceneObject* Engine::getSelectedObject() {
    if (selectedObjectId == -1) return nullptr;
    auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(),
        [this](const SceneObject& obj) { return obj.id == selectedObjectId; });
    return (it != sceneObjects.end()) ? &(*it) : nullptr;
}

glm::vec3 Engine::getSelectionCenterWorld(bool worldSpace) const {
    if (selectedObjectIds.empty()) return glm::vec3(0.0f);
    glm::vec3 acc(0.0f);
    int count = 0;
    auto findObj = [&](int id) -> const SceneObject* {
        auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(), [id](const SceneObject& o){ return o.id == id; });
        return it == sceneObjects.end() ? nullptr : &(*it);
    };
    for (int id : selectedObjectIds) {
        const SceneObject* o = findObj(id);
        if (!o) continue;
        acc += worldSpace ? o->position : glm::vec3(0.0f);
        count++;
    }
    if (count == 0) return glm::vec3(0.0f);
    return acc / (float)count;
}

void Engine::setPrimarySelection(int id, bool additive) {
    if (!additive) {
        selectedObjectIds.clear();
    }
    if (id >= 0) {
        if (std::find(selectedObjectIds.begin(), selectedObjectIds.end(), id) == selectedObjectIds.end()) {
            selectedObjectIds.push_back(id);
        }
        selectedObjectId = id;
        hierarchyRangeAnchorId = id;
    } else {
        selectedObjectIds.clear();
        selectedObjectId = -1;
        hierarchyRangeAnchorId = -1;
    }
}

void Engine::clearSelection() {
    selectedObjectIds.clear();
    selectedObjectId = -1;
    hierarchyRangeAnchorId = -1;
}

Camera Engine::makeCameraFromObject(const SceneObject& obj) const {
    Camera cam;
    cam.position = obj.position;
    cam.orthographic = isProject2DPipeline() || obj.camera.use2D;
    cam.pixelsPerUnit = std::max(1.0f, obj.camera.pixelsPerUnit);
    if (!cam.orthographic && obj.camera.projection == SceneCameraProjection::Orthographic) {
        cam.orthographic = true;
        cam.orthoSize = std::max(0.01f, obj.camera.orthoSize);
    }
    cam.renderShadows = obj.camera.renderShadows;
    cam.cullingMask = obj.camera.cullingMask;
    cam.solidBackground = (obj.camera.background == SceneCameraBackground::SolidColor);
    cam.backgroundColor = obj.camera.backgroundColor;
    glm::quat q = EulerXYZDegreesToQuat(obj.rotation);
    glm::mat3 rot = glm::mat3_cast(q);
    if (playerViewMotionCameraId == obj.id) {
        cam.position += rot * playerViewMotionOffset;
    }
    cam.front = glm::normalize(rot * glm::vec3(0.0f, 0.0f, -1.0f));
    cam.up = glm::normalize(rot * glm::vec3(0.0f, 1.0f, 0.0f));
    if (!std::isfinite(cam.front.x) || glm::length(cam.front) < 1e-3f) {
        cam.front = glm::vec3(0.0f, 0.0f, -1.0f);
    }
    if (!std::isfinite(cam.up.x) || glm::length(cam.up) < 1e-3f) {
        cam.up = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return cam;
}

Light2DPostFXSettings Engine::resolveWorld2DPostFx(const Camera& effectCamera) const {
    (void)effectCamera;
    Light2DPostFXSettings settings = world2DPostFx;
    if (!world2DPostFx.enabled) {
        settings.enabled = false;
        settings.ditherIntensity = 0.0f;
        settings.colorBits = 8;
        settings.darkAdjustment = 0.0f;
        settings.ditherScale = 1.0f;
        settings.pixelation = 0.0f;
    }

    settings.colorBits = std::clamp(settings.colorBits, 1, 8);
    settings.ditherIntensity = std::clamp(settings.ditherIntensity, 0.0f, 1.5f);
    settings.darkAdjustment = std::clamp(settings.darkAdjustment, 0.0f, 1.0f);
    settings.ditherScale = std::clamp(settings.ditherScale, 1.0f, 8.0f);
    settings.pixelation = std::clamp(settings.pixelation, 0.0f, 64.0f);
    settings.contrast = std::clamp(settings.contrast, 0.0f, 2.5f);
    settings.saturation = std::clamp(settings.saturation, 0.0f, 2.5f);
    settings.colorFilter = glm::clamp(settings.colorFilter, glm::vec3(0.0f), glm::vec3(2.0f));
    settings.vignetteIntensity = std::clamp(settings.vignetteIntensity, 0.0f, 1.0f);
    settings.vignetteSmoothness = std::clamp(settings.vignetteSmoothness, 0.05f, 1.0f);
    settings.chromaticAmount = std::clamp(settings.chromaticAmount, 0.0f, 0.05f);
    settings.sharpenStrength = std::clamp(settings.sharpenStrength, 0.0f, 2.0f);
    settings.grainAmount = std::clamp(settings.grainAmount, 0.0f, 0.4f);
    settings.scanlineIntensity = std::clamp(settings.scanlineIntensity, 0.0f, 1.0f);

    return settings;
}

Light2DPostFXSettings Engine::resolveWorld2DPostFx(const UIWorldCamera2D& effectCamera) const {
    Camera camera2D;
    camera2D.position = glm::vec3(effectCamera.position.x, effectCamera.position.y, 0.0f);
    camera2D.orthographic = true;
    camera2D.pixelsPerUnit = std::max(1.0f, effectCamera.zoom);
    return resolveWorld2DPostFx(camera2D);
}

const SceneObject* Engine::findPlayerCameraObject() const {
    for (const auto& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasCamera) continue;
        if (obj.camera.type == SceneCameraType::Player) {
            return &obj;
        }
    }
    return nullptr;
}
#pragma endregion

#pragma region Transform Helpers
namespace {
// Equivalent to glm::extractEulerAngleXYZ without depending on the experimental header.
glm::vec3 ExtractEulerXYZ(const glm::mat3& m, const glm::vec3* referenceDeg = nullptr) {
    const float C2 = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
    if (C2 < 1e-5f) {
        const float poleSign = (m[2][0] >= 0.0f) ? 1.0f : -1.0f;
        const float combined = std::atan2(poleSign * m[0][1], m[1][1]);
        const float z = referenceDeg ? glm::radians(referenceDeg->z) : 0.0f;
        return glm::vec3(combined - poleSign * z, poleSign * glm::half_pi<float>(), z);
    }

    float T1 = std::atan2(m[2][1], m[2][2]);
    float T2 = std::atan2(-m[2][0], C2);
    float S1 = std::sin(T1);
    float C1 = std::cos(T1);
    float T3 = std::atan2(S1 * m[0][2] - C1 * m[0][1], C1 * m[1][1] - S1 * m[1][2]);
    // GLM's extractEulerAngleXYZ returns (-T1, -T2, -T3)
    return glm::vec3(-T1, -T2, -T3);
}

glm::quat QuatFromEulerXYZ(const glm::vec3& deg) {
    glm::vec3 r = glm::radians(deg);
    glm::mat4 m(1.0f);
    m = glm::rotate(m, r.x, glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::rotate(m, r.y, glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::rotate(m, r.z, glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::quat_cast(glm::mat3(m));
}

fs::path findManagedProjectRoot(const fs::path& start) {
    std::error_code ec;
    fs::path current = start;
    for (int depth = 0; depth < 6 && !current.empty(); ++depth) {
        fs::path candidate = current / "Scripts" / "Managed" / "ModuCPP.csproj";
        if (fs::exists(candidate, ec)) {
            return current;
        }
        current = current.parent_path();
    }
    return {};
}

fs::path managedOutputPathFromProject(const fs::path& managedProject) {
    if (managedProject.empty()) return {};
    return managedProject.parent_path() / "bin" / "Debug" / "netstandard2.0" / "ModuCPP.dll";
}

bool isManagedGeneratedPath(const fs::path& path) {
    for (const auto& part : path) {
        std::string token = part.string();
        std::transform(token.begin(), token.end(), token.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (token == "obj" || token == "bin" || token == ".vs" || token == ".git") {
            return true;
        }
    }
    return false;
}

fs::path findEngineManagedRoot() {
    std::vector<fs::path> candidates;
    candidates.push_back(fs::current_path());
    candidates.push_back(fs::current_path().parent_path());
#if defined(__linux__)
    {
        std::error_code ec;
        fs::path exe = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            candidates.push_back(exe.parent_path());
            candidates.push_back(exe.parent_path().parent_path());
        }
    }
#elif defined(_WIN32)
    {
        wchar_t buffer[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (len > 0) {
            fs::path exe(buffer);
            candidates.push_back(exe.parent_path());
            candidates.push_back(exe.parent_path().parent_path());
        }
    }
#endif

    std::error_code ec;
    for (const auto& root : candidates) {
        if (root.empty()) continue;
        fs::path candidate = root / "Scripts" / "Managed" / "ModuCPP.cs";
        if (fs::exists(candidate, ec)) {
            return root;
        }
    }
    return {};
}

bool ensureProjectManagedCsproj(const fs::path& projectRoot, const fs::path& engineRoot, std::string& error) {
    if (projectRoot.empty() || engineRoot.empty()) {
        error = "Managed project setup failed: missing project or engine root.";
        return false;
    }
    fs::path managedDir = projectRoot / "Scripts" / "Managed";
    fs::path csprojPath = managedDir / "ModuCPP.csproj";
    std::error_code ec;
    if (fs::exists(csprojPath, ec)) return true;
    fs::create_directories(managedDir, ec);
    if (ec) {
        error = "Managed project setup failed: unable to create " + managedDir.string();
        return false;
    }

    fs::path engineApi = engineRoot / "Scripts" / "Managed" / "ModuCPP.cs";
    if (!fs::exists(engineApi, ec)) {
        error = "Managed project setup failed: engine ModuCPP.cs not found at " + engineApi.string();
        return false;
    }

    std::ofstream file(csprojPath);
    if (!file.is_open()) {
        error = "Managed project setup failed: unable to write " + csprojPath.string();
        return false;
    }

    file << "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
    file << "  <PropertyGroup>\n";
    file << "    <TargetFramework>netstandard2.0</TargetFramework>\n";
    file << "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n";
    file << "    <ImplicitUsings>enable</ImplicitUsings>\n";
    file << "    <Nullable>enable</Nullable>\n";
    file << "    <LangVersion>latest</LangVersion>\n";
    file << "    <GenerateRuntimeConfigurationFiles>false</GenerateRuntimeConfigurationFiles>\n";
    file << "    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n";
    file << "  </PropertyGroup>\n";
    file << "  <ItemGroup>\n";
    file << "    <Compile Include=\"*.cs\" />\n";
    file << "    <Compile Include=\"**/*.cs\" />\n";
    file << "    <Compile Include=\"../*.cs\" />\n";
    file << "    <Compile Include=\"../**/*.cs\" Exclude=\"../Managed/**\" />\n";
    file << "    <Compile Include=\"" << engineApi.generic_string() << "\" Link=\"ModuCPP.cs\" />\n";
    file << "  </ItemGroup>\n";
    file << "</Project>\n";
    return true;
}

std::string TrimScriptName(const std::string& value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string BuildScriptIdentifier(const std::string& requestedName) {
    const std::string trimmed = TrimScriptName(requestedName);
    if (trimmed.empty()) {
        return "NewScript";
    }

    std::string identifier;
    identifier.reserve(trimmed.size());
    bool capitalizeNext = true;
    for (char c : trimmed) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && c != '_') {
            capitalizeNext = true;
            continue;
        }

        if (identifier.empty() && std::isdigit(uc) != 0) {
            identifier.push_back('_');
        }

        if (std::isalpha(uc) != 0) {
            identifier.push_back(static_cast<char>(capitalizeNext ? std::toupper(uc) : c));
        } else {
            identifier.push_back(c);
        }
        capitalizeNext = (c == '_');
    }

    if (identifier.empty()) {
        return "NewScript";
    }
    return identifier;
}

fs::path MakeUniqueSiblingPath(const fs::path& basePath) {
    std::error_code ec;
    if (!fs::exists(basePath, ec)) {
        return basePath;
    }

    const fs::path parent = basePath.parent_path();
    const std::string stem = basePath.stem().string();
    const std::string ext = basePath.extension().string();
    for (int index = 1; index < 10000; ++index) {
        fs::path candidate = parent / (stem + "_" + std::to_string(index) + ext);
        if (!fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return parent / (stem + "_" + std::to_string(std::time(nullptr)) + ext);
}

bool WriteTextFile(const fs::path& path, const std::string& contents, std::string& error) {
    std::ofstream file(path);
    if (!file.is_open()) {
        error = "unable to open " + path.string() + " for writing";
        return false;
    }
    file << contents;
    if (!file.good()) {
        error = "failed while writing " + path.string();
        return false;
    }
    return true;
}

std::string BuildScriptTemplateContents(ScriptScaffoldKind kind, const std::string& className) {
    switch (kind) {
        case ScriptScaffoldKind::ModuMako:
            return
                "using ModuMAKO;\n"
                "using ModuEngine;\n"
                "using ModuInput;\n"
                "\n"
                "script \"" + className + "\" : ModuNode;\n"
                "\n"
                "speed = 1;\n"
                "\n"
                "Begin() {\n"
                "    Ensure.obj;\n"
                "}\n"
                "\n"
                "TickUpdate() {\n"
                "}\n";
        case ScriptScaffoldKind::ModuCpp:
            return
                "#include \"ModuCPP\"\n"
                "\n"
                "public class " + className + " : ModuNode {\n"
                "    public float interval = 1.0f;\n"
                "    private float timer = 0.0f;\n"
                "\n"
                "    void Begin() to timer.Start(interval);\n"
                "\n"
                "    void TickUpdate() {\n"
                "        if (!timer.Ready()) return;\n"
                "        // Interval work goes here.\n"
                "    }\n"
                "}\n";
        case ScriptScaffoldKind::Cpp:
            return
                "#include \"ScriptRuntime.h\"\n"
                "#include \"SceneObject.h\"\n"
                "#include \"ThirdParty/ModuGUI/imgui.h\"\n"
                "\n"
                "extern \"C\" void Script_OnInspector(ScriptContext& ctx) {\n"
                "    ImGui::TextUnformatted(\"" + className + "\");\n"
                "}\n"
                "\n"
                "void Begin(ScriptContext& ctx, float /*deltaTime*/) {\n"
                "}\n"
                "\n"
                "void TickUpdate(ScriptContext& ctx, float /*deltaTime*/) {\n"
                "}\n";
        case ScriptScaffoldKind::C:
            return
                "#include \"ScriptRuntimeCAPI.h\"\n"
                "\n"
                "void Modu_OnInspector(ModuScriptContext* ctx) {\n"
                "    (void)ctx;\n"
                "}\n"
                "\n"
                "void Modu_TickUpdate(ModuScriptContext* ctx, float deltaTime) {\n"
                "    (void)deltaTime;\n"
                "    ModuVec3 pos = Modu_GetPosition(ctx);\n"
                "    pos.y += 0.0f;\n"
                "    Modu_SetPosition(ctx, pos);\n"
                "}\n";
        case ScriptScaffoldKind::CSharp:
            return
                "using System;\n"
                "\n"
                "namespace ModuCPP;\n"
                "\n"
                "public class " + className + "\n"
                "{\n"
                "    public void Begin(IntPtr ctx, float deltaTime)\n"
                "    {\n"
                "    }\n"
                "\n"
                "    public void TickUpdate(IntPtr ctx, float deltaTime)\n"
                "    {\n"
                "        _ = deltaTime;\n"
                "        _ = new Context(ctx);\n"
                "    }\n"
                "}\n";
    }
    return {};
}
}

glm::vec3 ChooseContinuousEulerDegrees(const glm::vec3& primaryDeg, const glm::vec3& referenceDeg) {
    auto unwrapNear = [](float angle, float reference) {
        float result = angle;
        while (result - reference > 180.0f) result -= 360.0f;
        while (reference - result > 180.0f) result += 360.0f;
        return result;
    };

    const glm::vec3 alternateDeg(primaryDeg.x + 180.0f, 180.0f - primaryDeg.y, primaryDeg.z + 180.0f);

    glm::vec3 primaryNear(unwrapNear(primaryDeg.x, referenceDeg.x),
                          unwrapNear(primaryDeg.y, referenceDeg.y),
                          unwrapNear(primaryDeg.z, referenceDeg.z));
    glm::vec3 alternateNear(unwrapNear(alternateDeg.x, referenceDeg.x),
                            unwrapNear(alternateDeg.y, referenceDeg.y),
                            unwrapNear(alternateDeg.z, referenceDeg.z));

    auto travel = [&](const glm::vec3& candidate) {
        return std::abs(candidate.x - referenceDeg.x) +
               std::abs(candidate.y - referenceDeg.y) +
               std::abs(candidate.z - referenceDeg.z);
    };

    return (travel(alternateNear) < travel(primaryNear)) ? alternateNear : primaryNear;
}

void Engine::DecomposeMatrix(const glm::mat4& matrix, glm::vec3& pos, glm::vec3& rot, glm::vec3& scale,
                             const glm::vec3* referenceScale, const glm::vec3* referenceRotationDeg) {
    pos = glm::vec3(matrix[3]);
    scale.x = glm::length(glm::vec3(matrix[0]));
    scale.y = glm::length(glm::vec3(matrix[1]));
    scale.z = glm::length(glm::vec3(matrix[2]));

    // Column lengths are always positive, so a mirrored matrix (odd number of negative
    const bool mirrored = glm::determinant(glm::mat3(matrix)) < 0.0f;
    if (mirrored) {
        int flipAxis = 0;
        if (referenceScale) {
            for (int axis = 0; axis < 3; ++axis) {
                if ((*referenceScale)[axis] < 0.0f) {
                    flipAxis = axis;
                    break;
                }
            }
        }
        scale[flipAxis] = -scale[flipAxis];
    }

    glm::mat3 rotMat(matrix);
    if (scale.x != 0.0f) rotMat[0] /= scale.x;
    if (scale.y != 0.0f) rotMat[1] /= scale.y;
    if (scale.z != 0.0f) rotMat[2] /= scale.z;
    rotMat[0] = glm::normalize(rotMat[0]);
    rotMat[1] = glm::normalize(rotMat[1] - rotMat[0] * glm::dot(rotMat[0], rotMat[1]));
    rotMat[2] = glm::normalize(glm::cross(rotMat[0], rotMat[1]));

    // Use explicit XYZ extraction so yaw isn't clamped to [-90, 90] like glm::yaw/pitch/roll.
    rot = ExtractEulerXYZ(rotMat, referenceRotationDeg);
    if (referenceRotationDeg) {
        rot = glm::radians(ChooseContinuousEulerDegrees(glm::degrees(rot), *referenceRotationDeg));
    }
}

glm::mat4 Engine::ComposeTransform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale) {
    glm::mat4 m(1.0f);
    m = glm::translate(m, position);
    m *= glm::mat4_cast(rotation);
    m = glm::scale(m, scale);
    return m;
}

glm::mat4 Engine::ComposeTransform(const glm::vec3& position, const glm::vec3& rotationDeg, const glm::vec3& scale) {
    return ComposeTransform(position, QuatFromEulerXYZ(rotationDeg), scale);
}
#pragma endregion

#pragma region Undo / Redo
Engine::SceneSnapshot Engine::captureSceneSnapshot() const {
    SceneSnapshot snap;
    snap.objects = sceneObjects;
    snap.selectedIds = selectedObjectIds;
    snap.nextId = nextObjectId;
    snap.meshEditMode = meshEditMode;
    snap.meshEditLoaded = meshEditLoaded;
    snap.meshEditDirty = meshEditDirty;
    snap.meshEditExtrudeMode = meshEditExtrudeMode;
    snap.meshEditAutoUV = meshEditAutoUV;
    snap.meshEditTriangleSelection = meshEditTriangleSelection;
    snap.meshEditAutoObjectId = meshEditAutoObjectId;
    snap.meshEditPath = meshEditPath;
    snap.meshEditAsset = meshEditAsset;
    snap.meshEditSelectedVertices = meshEditSelectedVertices;
    snap.meshEditSelectedEdges = meshEditSelectedEdges;
    snap.meshEditSelectedFaces = meshEditSelectedFaces;
    snap.meshEditActiveMaterialSlot = meshEditActiveMaterialSlot;
    snap.meshEditSelectionMode = static_cast<int>(meshEditSelectionMode);
    return snap;
}

void Engine::restoreSceneSnapshot(SceneSnapshot snap) {
    sceneObjects = std::move(snap.objects);
    selectedObjectIds = std::move(snap.selectedIds);
    selectedObjectId = selectedObjectIds.empty() ? -1 : selectedObjectIds.back();
    nextObjectId = snap.nextId;
    meshEditMode = snap.meshEditMode;
    meshEditLoaded = snap.meshEditLoaded;
    meshEditDirty = snap.meshEditDirty;
    meshEditExtrudeMode = snap.meshEditExtrudeMode;
    meshEditAutoUV = snap.meshEditAutoUV;
    meshEditTriangleSelection = snap.meshEditTriangleSelection;
    meshEditAutoObjectId = snap.meshEditAutoObjectId;
    meshEditPath = std::move(snap.meshEditPath);
    meshEditAsset = std::move(snap.meshEditAsset);
    meshEditSelectedVertices = std::move(snap.meshEditSelectedVertices);
    meshEditSelectedEdges = std::move(snap.meshEditSelectedEdges);
    meshEditSelectedFaces = std::move(snap.meshEditSelectedFaces);
    meshEditActiveMaterialSlot = snap.meshEditActiveMaterialSlot;
    if (snap.meshEditSelectionMode < static_cast<int>(MeshEditSelectionMode::Object) ||
        snap.meshEditSelectionMode > static_cast<int>(MeshEditSelectionMode::UV)) {
        meshEditSelectionMode = MeshEditSelectionMode::Object;
    } else {
        meshEditSelectionMode = static_cast<MeshEditSelectionMode>(snap.meshEditSelectionMode);
    }
    if (meshEditLoaded && !meshEditPath.empty()) {
        SceneObject* meshTarget = getSelectedObject();
        if (!meshTarget || !IsMeshEditablePath(meshTarget->meshPath) ||
            meshTarget->meshPath != meshEditPath) {
            meshTarget = nullptr;
            for (SceneObject& obj : sceneObjects) {
                if (IsMeshEditablePath(obj.meshPath) && obj.meshPath == meshEditPath) {
                    meshTarget = &obj;
                    break;
                }
            }
        }
        if (meshTarget) {
            syncMeshEditToGPU(meshTarget);
        }
    }

    sceneObjectIndexById.clear();
    sceneObjectIndexData = nullptr;
    sceneObjectIndexCount = 0;
    markRuntimeScriptBindingsDirty();
    aiAgentRuntimeStates.clear();
    activePlayerId = -1;
    playerControllerGroundProbeDebug = {};
    updateHierarchyWorldTransforms();
    gizmoHistoryCaptured = false;
    gizmoDragActive = false;
    gizmoDragPivotShift = glm::vec3(0.0f);
    gizmoDragStartTransforms.clear();
    gizmoDragLastAppliedTransforms.clear();
    worldUiGizmoHistoryCaptured = false;
    gameUiGizmoHistoryCaptured = false;
    worldUiRectGizmoSnapshots.clear();
    gameUiRectGizmoSnapshots.clear();
    worldUiRectGizmoModel = glm::mat4(1.0f);
    gameUiRectGizmoModel = glm::mat4(1.0f);
    worldUiRectGizmoStartMouse = ImVec2(0.0f, 0.0f);
    gameUiRectGizmoStartMouse = ImVec2(0.0f, 0.0f);
#if !MODULARITY_RUNTIME_ONLY
    applyMapSectorVisibility();
#endif
}

void Engine::pushUndoSnapshot(SceneSnapshot snap, const char* /*reason*/) {
    undoStack.push_back(std::move(snap));
    if (undoStack.size() > 64) {
        undoStack.erase(undoStack.begin());
    }
    redoStack.clear();
}

void Engine::recordState(const char* reason) {
    pushUndoSnapshot(captureSceneSnapshot(), reason);
}

void Engine::capturePlayModeSnapshot() {
    playerControllerRuntimeStates.clear();
    playerControllerGroundProbeDebug = {};
    playerViewMotionCameraId = -1;
    playerViewMotionOffset = glm::vec3(0.0f);
    playModeSnapshot.scene = captureSceneSnapshot();
    playModeSnapshot.hadUnsavedChanges = projectManager.currentProject.hasUnsavedChanges;
    playModeSnapshot.valid = true;
}

namespace {
void ResetParticleSystem2DRuntime(SceneObject& obj) {
    if (!obj.hasParticleSystem2D) {
        return;
    }
    ParticleRuntime::ResetRuntime(obj.particleSystem2D);
}

void ResetParticleSystem2DRuntimes(std::vector<SceneObject>& objects) {
    for (SceneObject& obj : objects) {
        ResetParticleSystem2DRuntime(obj);
    }
}
} // namespace

void Engine::restorePlayModeSnapshot() {
    if (!playModeSnapshot.valid) {
        return;
    }

    restoreSceneSnapshot(std::move(playModeSnapshot.scene));
    ResetParticleSystem2DRuntimes(sceneObjects);
    projectManager.currentProject.hasUnsavedChanges = playModeSnapshot.hadUnsavedChanges;
    playerControllerRuntimeStates.clear();
    playerControllerGroundProbeDebug = {};
    playerViewMotionCameraId = -1;
    playerViewMotionOffset = glm::vec3(0.0f);

    playModeSnapshot = {};
}

void Engine::undo() {
    if (undoStack.empty()) return;

#if !MODULARITY_RUNTIME_ONLY
    if (undoStack.back().kind == SceneSnapshot::Kind::AssemblageCells) {
        SceneSnapshot snap = std::move(undoStack.back());
        undoStack.pop_back();
        applyAssemblageEdit(snap.assemblageEdit, false);
        redoStack.push_back(std::move(snap));
        projectManager.currentProject.hasUnsavedChanges = true;
        return;
    }
#endif

    SceneSnapshot current = captureSceneSnapshot();

    SceneSnapshot snap = undoStack.back();
    undoStack.pop_back();

    redoStack.push_back(std::move(current));
    restoreSceneSnapshot(std::move(snap));
    projectManager.currentProject.hasUnsavedChanges = true;
}

void Engine::redo() {
    if (redoStack.empty()) return;

#if !MODULARITY_RUNTIME_ONLY
    if (redoStack.back().kind == SceneSnapshot::Kind::AssemblageCells) {
        SceneSnapshot snap = std::move(redoStack.back());
        redoStack.pop_back();
        applyAssemblageEdit(snap.assemblageEdit, true);
        undoStack.push_back(std::move(snap));
        projectManager.currentProject.hasUnsavedChanges = true;
        return;
    }
#endif

    SceneSnapshot current = captureSceneSnapshot();

    SceneSnapshot snap = redoStack.back();
    redoStack.pop_back();

    undoStack.push_back(std::move(current));
    restoreSceneSnapshot(std::move(snap));
    projectManager.currentProject.hasUnsavedChanges = true;
}
#pragma endregion

fs::path resolveScriptsConfigPath(const Project& project) {
    std::error_code ec;
    if (!project.scriptsConfigPath.empty() && fs::exists(project.scriptsConfigPath, ec)) {
        return project.scriptsConfigPath;
    }
    fs::path lower = project.projectPath / "scripts.modu";
    if (fs::exists(lower, ec)) {
        return lower;
    }
    return project.projectPath / "Scripts.modu";
}

#pragma region Engine Lifecycle
bool Engine::hasInstalledPackage(const char* id) const {
    return id && *id && packageManager.isInstalled(id);
}

bool Engine::hasSpriteEditorPackage() const {
    return hasInstalledPackage("moduengine.sprite-editor");
}

bool Engine::hasSpritesheetPackage() const {
    return hasInstalledPackage("moduengine.spritesheet");
}

bool Engine::has2DWorldPackage() const {
    return hasInstalledPackage("moduengine.2d-world");
}

bool Engine::hasMeshBuilderPackage() const {
    return hasInstalledPackage("moduengine.mesh-builder");
}

bool Engine::hasScriptingWindowPackage() const {
    return hasInstalledPackage("moduengine.scripting-window");
}

#if MODULARITY_RUNTIME_ONLY
void Engine::openScriptInEditor(const fs::path& path) {
    (void)path;
}
#endif

bool Engine::hasVulkanPipelinePackage() const {
    return hasInstalledPackage("moduengine.vulkan-pipeline");
}

void Engine::clampOptionalPackageState(bool logMissingRenderer) {
    if (!hasMeshBuilderPackage()) {
        showMeshBuilder = false;
        meshEditMode = false;
    }

    if (!hasScriptingWindowPackage()) {
        showScriptingWindow = false;
        if (currentWorkspace == WorkspaceMode::Scripting) {
            currentWorkspace = WorkspaceMode::Default;
        }
        workspaceTabVisible[2] = false;
    } else {
        workspaceTabVisible[2] = true;
    }

    if (!hasSpriteEditorPackage()) {
        showPixelSpriteEditorWindow = false;
    }

    if (!hasSpritesheetPackage()) {
        showImportSpriteSheetDialog = false;
    }

    if (projectManager.currentProject.isLoaded &&
        projectManager.currentProject.rendererBackend == Modularity::GraphicsBackend::Vulkan &&
        !hasVulkanPipelinePackage()) {
        projectManager.currentProject.rendererBackend = Modularity::GraphicsBackend::OpenGL;
        projectManager.currentProject.saveProjectFile();
        if (logMissingRenderer) {
            addConsoleMessage("Project requested Vulkan, but moduengine.vulkan-pipeline is not installed. Falling back to OpenGL.",
                              ConsoleMessageType::Warning);
        }
    }
}

Modularity::GraphicsBackend Engine::resolveRequestedBackend() const {
    const char* value = std::getenv("MODULARITY_RENDER_BACKEND");
    if (!value || !*value) {
        value = std::getenv("MODULARITY_GFX_API");
    }
    if (!value || !*value) {
        return Modularity::GraphicsBackend::OpenGL;
    }

    Modularity::GraphicsBackend backend = Modularity::GraphicsBackendFromString(value);
#if !MODULARITY_HAS_VULKAN
    if (backend == Modularity::GraphicsBackend::Vulkan) {
        std::cerr << "[WARN] Vulkan was requested but this build has no Vulkan support. Falling back to OpenGL.\n";
        return Modularity::GraphicsBackend::OpenGL;
    }
#endif
    return backend;
}

void Engine::applySceneTimeOfDay(float timeOfDay) {
    sceneTimeOfDay = normalizeTimeOfDay(timeOfDay);

    if (Skybox* skybox = renderer.getSkybox()) {
        skybox->setTimeOfDay(sceneTimeOfDay);
    }

    if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
        vulkanRenderer->setSkyboxTimeOfDay(sceneTimeOfDay);
    }
}

float Engine::getSceneTimeOfDay() {
    if (!usingVulkan()) {
        if (Skybox* skybox = renderer.getSkybox()) {
            sceneTimeOfDay = normalizeTimeOfDay(skybox->getTimeOfDay());
        }
    }
    return sceneTimeOfDay;
}

void Engine::applySceneSkyboxSettings(const SkyboxSettings& settings) {
    sceneSkyboxSettings = settings;
    sceneSkyboxSettings.scrollingRepeatX = std::max(0.01f, sceneSkyboxSettings.scrollingRepeatX);
    sceneSkyboxSettings.scrollingRepeatY = std::max(0.01f, sceneSkyboxSettings.scrollingRepeatY);
    sceneSkyboxSettings.scrollingLookSensitivity = std::max(0.0f, sceneSkyboxSettings.scrollingLookSensitivity);
    sceneSkyboxSettings.scrollingVerticalInfluence = std::clamp(sceneSkyboxSettings.scrollingVerticalInfluence, 0.0f, 1.0f);
    sceneSkyboxSettings.environmentReflectionIntensity = std::clamp(sceneSkyboxSettings.environmentReflectionIntensity, 0.0f, 2.0f);
    sceneSkyboxSettings.reflectionDistanceFadeStart = std::max(0.0f, sceneSkyboxSettings.reflectionDistanceFadeStart);
    sceneSkyboxSettings.reflectionDistanceFadeEnd = std::max(sceneSkyboxSettings.reflectionDistanceFadeStart + 0.01f, sceneSkyboxSettings.reflectionDistanceFadeEnd);
    sceneSkyboxSettings.fogMode = std::clamp(sceneSkyboxSettings.fogMode, 0, 2);
    sceneSkyboxSettings.fogStart = std::max(0.0f, sceneSkyboxSettings.fogStart);
    sceneSkyboxSettings.fogEnd = std::max(sceneSkyboxSettings.fogStart + 0.01f, sceneSkyboxSettings.fogEnd);
    sceneSkyboxSettings.fogDensity = std::clamp(sceneSkyboxSettings.fogDensity, 0.0f, 1.0f);
    sceneSkyboxSettings.fogHeightFalloff = std::clamp(sceneSkyboxSettings.fogHeightFalloff, 0.0f, 1.0f);

    if (Skybox* skybox = renderer.getSkybox()) {
        skybox->setSettings(sceneSkyboxSettings);
    }

    if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
        vulkanRenderer->setSkyboxSettings(sceneSkyboxSettings);
    }
}

SkyboxSettings Engine::getSceneSkyboxSettings() const {
    return sceneSkyboxSettings;
}

void Engine::getSceneViewportInternalResolution(int& outWidth, int& outHeight) const {
    if (!playerMode && viewportWidth > 0 && viewportHeight > 0) {
        outWidth = std::clamp(viewportWidth, 1, 8192);
        outHeight = std::clamp(viewportHeight, 1, 8192);
        return;
    }

    outWidth = std::clamp(sceneViewportRenderWidth, 64, 8192);
    outHeight = std::clamp(sceneViewportRenderHeight, 64, 8192);
}

float Engine::getSceneViewportInternalAspect() const {
    int width = 1600;
    int height = 900;
    getSceneViewportInternalResolution(width, height);
    return static_cast<float>(width) / static_cast<float>(std::max(1, height));
}

void Engine::getRuntimeInternalResolution(int& outWidth, int& outHeight) const {
    static int _lastIndex = -2;
    static int _lastNative = -1;
    static int _lastW = -1, _lastH = -1;
    auto _finish = [&]() {
        const float renderScale = std::clamp(
            projectManager.currentProject.graphicsSettings.renderResolutionScale, 0.25f, 2.0f);
        if (renderScale != 1.0f) {
            outWidth = std::clamp(static_cast<int>(std::lround(outWidth * renderScale)), 64, 8192);
            outHeight = std::clamp(static_cast<int>(std::lround(outHeight * renderScale)), 64, 8192);
        }
        const int curNative = (int)projectManager.currentProject.playerSettings.nativeDisplayResolution;
        if (_lastIndex != gameViewportResolutionIndex ||
            _lastNative != curNative ||
            _lastW != outWidth || _lastH != outHeight) {
            std::fprintf(stderr, "[Runtime] resolution index=%d native=%d -> %dx%d (custom=%dx%d)\n",
                         gameViewportResolutionIndex, curNative,
                         outWidth, outHeight,
                         gameViewportCustomWidth, gameViewportCustomHeight);
            _lastIndex = gameViewportResolutionIndex;
            _lastNative = curNative;
            _lastW = outWidth;
            _lastH = outHeight;
        }
    };

    if (projectManager.currentProject.playerSettings.nativeDisplayResolution) {
        int w = 0, h = 0;
#ifdef __ANDROID__
        Modularity::AndroidRuntime::GetSurfaceSize(&w, &h);
#else
        if (editorWindow) glfwGetFramebufferSize(editorWindow, &w, &h);
#endif
        outWidth  = (w > 0) ? w : kRuntimeInternalWidth;
        outHeight = (h > 0) ? h : kRuntimeInternalHeight;
        _finish();
        return;
    }

    switch (gameViewportResolutionIndex) {
        case 1:
            outWidth = 1920;
            outHeight = 1080;
            break;
        case 2:
            outWidth = 1280;
            outHeight = 720;
            break;
        case 3:
            outWidth = 2560;
            outHeight = 1440;
            break;
        case 4:
            outWidth = std::clamp(gameViewportCustomWidth, 64, 8192);
            outHeight = std::clamp(gameViewportCustomHeight, 64, 8192);
            break;
        case 5: {
            int w = 0, h = 0;
#ifdef __ANDROID__
            Modularity::AndroidRuntime::GetSurfaceSize(&w, &h);
#else
            if (editorWindow) glfwGetFramebufferSize(editorWindow, &w, &h);
#endif
            outWidth  = (w > 0) ? w : kRuntimeInternalWidth;
            outHeight = (h > 0) ? h : kRuntimeInternalHeight;
            break;
        }
        case 0:
        default:
            outWidth = kRuntimeInternalWidth;
            outHeight = kRuntimeInternalHeight;
            break;
    }
    _finish();
}

float Engine::getRuntimeInternalAspect() const {
    int width = kRuntimeInternalWidth;
    int height = kRuntimeInternalHeight;
    getRuntimeInternalResolution(width, height);
    return static_cast<float>(width) / static_cast<float>(std::max(1, height));
}

void Engine::applyProjectGraphicsToRenderer() {
    const ProjectGraphicsSettings& graphics = projectManager.currentProject.graphicsSettings;

    int lightBudget = 20;
    int directionalAllowance = 1;
    ProjectRenderingPathCaps(graphics.renderingPath, lightBudget, directionalAllowance);
    renderer.setSceneLightBudget(lightBudget, directionalAllowance);
    if (playerMode) {
        renderer.setMaxRealtimeLights(kRendererMaxRealtimeLights);
    }

    renderer.setMainLightEnabled(graphics.mainLightEnabled);
    renderer.setMainLightShadows(graphics.mainLightCastShadows);
    renderer.setAdditionalLightsEnabled(graphics.additionalLightsEnabled);
    renderer.setAdditionalLightsShadows(graphics.additionalLightsCastShadows);
    renderer.setSpecularEnabled(graphics.specularEnabled);
    renderer.setSoftShadowsAllowed(graphics.softShadows);
    renderer.setShadowMaxDistance(graphics.shadowMaxDistance);

    RendererColorPrecision precision = RendererColorPrecision::Auto;
    if (!graphics.hdr || graphics.colorResolution == ProjectColorResolution::Color8) {
        precision = RendererColorPrecision::SDR8;
    } else if (graphics.colorResolution == ProjectColorResolution::Color16F) {
        precision = RendererColorPrecision::HDR16F;
    }
    renderer.setColorPrecision(precision);

    renderer.setDefaultTextureFormatPolicy(TextureFormatPolicyFromString(graphics.defaultTextureFormat));
    renderer.setMaxTextureDimension(graphics.maxTextureSize);
    renderer.setShadowQuality(graphics.shadowFilterQuality, graphics.shadowLodDistance,
                              graphics.shadowContactHardening);

    RenderOptimizationSettings optimization;
    optimization.strategy = graphics.optimizationStrategy == ProjectOptimizationStrategy::Automatic
        ? RenderOptimizationStrategy::Automatic
        : RenderOptimizationStrategy::Manual;
    optimization.cpuFrustumCulling = graphics.optCpuFrustumCulling;
    optimization.multiDrawIndirect = graphics.optMultiDrawIndirect;
    optimization.gpuDrivenCulling = graphics.optGpuDrivenCulling;
    optimization.lodOptimization = graphics.optLod;
    optimization.lodMinScreenSizePx = graphics.optLodMinScreenSize;
    optimization.hiZOcclusionCulling = graphics.optHiZOcclusionCulling;
    optimization.materialSorting = graphics.optMaterialSorting;
    optimization.staticObjectCaching = graphics.optStaticObjectCaching;
    renderer.setOptimizationSettings(optimization);

    applyPresentationSettings();
}

void Engine::applyPresentationSettings() {
    if (usingVulkan()) return;
    if (!editorWindow) return;

    const bool xrPacesFrames = xrSystem.isRunning();
    const bool wantVSync = !xrPacesFrames &&
        (playerMode ? projectManager.currentProject.graphicsSettings.vsync
                    : editorVSyncEnabled);
    const int desiredInterval = wantVSync ? 1 : 0;
    if (desiredInterval == presentSwapIntervalApplied) return;

#ifndef __ANDROID__
    glfwMakeContextCurrent(editorWindow);
    glfwSwapInterval(desiredInterval);
#endif
    presentSwapIntervalApplied = desiredInterval;
    framePacingDeadline = 0.0;
    framePacingInterval = 0.0;
}

void Engine::paceFrame(double frameStart) {
    if (xrSystem.isRunning()) {
        framePacingDeadline = framePacingInterval = 0.0;
        return; // xrWaitFrame owns headset pacing, independent of the desktop cap.
    }
    const bool presentationIsVSyncPaced =
        !usingVulkan() &&
        (playerMode
             ? projectManager.currentProject.graphicsSettings.vsync
             : editorVSyncEnabled);
    if (presentationIsVSyncPaced) {
        framePacingDeadline = 0.0;
        framePacingInterval = 0.0;
        return;
    }

    double interval = 0.0;
    if (fpsCapEnabled && fpsCap > 1.0f) {
        interval = 1.0 / static_cast<double>(fpsCap);
    } else if (playerMode) {
        const ProjectGraphicsSettings& graphics = projectManager.currentProject.graphicsSettings;
        if (!graphics.vsync && graphics.targetFps > 0) {
            interval = 1.0 / static_cast<double>(graphics.targetFps);
        }
    }

    if (interval <= 0.0) {
        framePacingDeadline = 0.0;
        framePacingInterval = 0.0;
        return;
    }

    const double now = glfwGetTime();
    if (framePacingDeadline <= 0.0 || interval != framePacingInterval ||
        now > framePacingDeadline + interval) {
        framePacingInterval = interval;
        framePacingDeadline = frameStart + interval;
    } else {
        framePacingDeadline += interval;
    }

    while (true) {
        const double remainingMs = (framePacingDeadline - glfwGetTime()) * 1000.0;
        if (remainingMs <= 0.0) break;

        if (remainingMs > framePacingSleepOvershootMs) {
            const double requestedMs =
                remainingMs - framePacingSleepOvershootMs;
            const auto sleepStart = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(
                std::chrono::duration<double, std::milli>(requestedMs));
            const double actualMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - sleepStart).count();
            const double overshootMs =
                std::clamp(actualMs - requestedMs, 0.0, 2.0);
            framePacingSleepOvershootMs = std::clamp(
                framePacingSleepOvershootMs * 0.9 + overshootMs * 0.1,
                0.1,
                2.0);
        } else {
            std::this_thread::yield();
        }
    }
}

void Engine::onWindowResized(int width, int height) {
    if (width <= 0 || height <= 0) return;
    viewportWidth = width;
    viewportHeight = height;

    if (usingVulkan()) {
        if (vulkanRenderer) {
            vulkanRenderer->notifyResize();
        }
        return;
    }

    if (rendererInitialized) {
        int targetWidth = width;
        int targetHeight = height;
        if (!playerMode) {
            getSceneViewportInternalResolution(targetWidth, targetHeight);
        }
        renderer.resize(targetWidth, targetHeight);
    }
}

bool Engine::init() {
#if !MODULARITY_RUNTIME_ONLY
    // Read preview mode before any OpenXR initialization (including --vr-editor).
    if(const char* value=std::getenv("MODULARITY_VR_EDITOR");value && *value && *value!='0')vrEditor.requested=true;
    if(const char* value=std::getenv("MODULARITY_VR_EDITOR_PREVIEW");value && *value && *value!='0')vrEditor.requested=vrEditor.preview=true;
#endif
#if defined(_WIN32) && !defined(__ANDROID__)
    {
        // Test the STREAM, not the OS handle.
        //
        // GetStdHandle(STD_ERROR_HANDLE) still reports NULL in a windowed build even
        // after CrashReporter::Install has given stderr a real file, so checking the
        // handle here reopened a stream that was already perfectly good - throwing away
        // the redirect that had just been set up.
        //
        // That was worse than wasteful, because freopen closes the stream *before* it
        // tries to reopen, and this used a fixed relative filename. A second editor
        // process - whose first copy already holds modularity-boot.log open - failed the
        // reopen and was left with a CLOSED stderr. Measured: an unattached stderr
        // (fd=-2) writes harmlessly, but a closed one makes the very next fprintf trip
        // the CRT invalid-parameter handler, which the crash reporter turns into a fatal
        // "CRT invalid parameter" before the window ever appears. _initMark's timing
        // line was the trigger.
        //
        // _fileno is what fprintf itself ends up using, so it is the honest question to
        // ask, and it is already true when the crash reporter has done its job.
        if (stderr == nullptr || _fileno(stderr) < 0) {
            // Per-process filename, so a second instance does not fight the first over
            // the same log. The plain name is kept for the common single-instance case
            // because it is what the docs and existing habits point at.
            const std::string bootLogName =
                "modularity-boot.log";
            FILE* redirected = nullptr;
            bool opened = (freopen_s(&redirected, bootLogName.c_str(), "w", stderr) == 0 &&
                           redirected != nullptr);
            if (!opened) {
                const std::string fallbackName =
                    "modularity-boot-" + std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) +
                    ".log";
                opened = (freopen_s(&redirected, fallbackName.c_str(), "w", stderr) == 0 &&
                          redirected != nullptr);
            }
            if (opened) {
                std::setvbuf(stderr, nullptr, _IONBF, 0);
                std::fprintf(stderr, "[Modularity] Diagnostic log. Delete this file freely.\n");
            } else {
                // Both reopens failed, and freopen has already taken the original stream
                // down with it. NUL always opens; a sink that discards beats a closed
                // FILE* that kills the process on the next write.
                FILE* nullSink = nullptr;
                freopen_s(&nullSink, "NUL", "w", stderr);
            }
        }
    }
#endif
    const auto __initStart = std::chrono::steady_clock::now();
    auto __initMark = [&](const char* tag) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - __initStart).count();
        std::fprintf(stderr, "[ModuTimer] init/%s %.1f ms\n", tag, ms);
    };
    graphicsBackend = resolveRequestedBackend();
    editorWindow = window.makeWindow(graphicsBackend);
    if (!editorWindow && graphicsBackend == Modularity::GraphicsBackend::Vulkan) {
        std::cerr << "[WARN] Vulkan window creation failed. Falling back to OpenGL.\n";
        graphicsBackend = Modularity::GraphicsBackend::OpenGL;
        editorWindow = window.makeWindow(graphicsBackend);
    }
    if (!editorWindow) {
        return false;
    }

    glfwSetWindowUserPointer(editorWindow, this);
    glfwSetWindowSizeCallback(editorWindow, window_size_callback);

    auto mouse_cb = [](GLFWwindow* window, double xpos, double ypos) {
        auto* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
        (void)window;
        (void)xpos;
        (void)ypos;
        (void)engine;
    };
    glfwSetCursorPosCallback(editorWindow, mouse_cb);

    auto drop_cb = [](GLFWwindow* window, int pathCount, const char* paths[]) {
        auto* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
        if (!engine || pathCount <= 0 || !paths) return;

        double mouseX = 0.0;
        double mouseY = 0.0;
        glfwGetCursorPos(window, &mouseX, &mouseY);

        engine->pendingExternalFileDrops.reserve(engine->pendingExternalFileDrops.size() + static_cast<size_t>(pathCount));
        for (int i = 0; i < pathCount; ++i) {
            if (!paths[i] || !*paths[i]) continue;
            Engine::ExternalFileDropEvent evt;
            evt.path = fs::path(paths[i]);
            evt.mouseX = mouseX;
            evt.mouseY = mouseY;
            engine->pendingExternalFileDrops.push_back(std::move(evt));
        }
    };
    glfwSetDropCallback(editorWindow, drop_cb);

    loadAutoStartConfig();
    if (!startupProjectPath.empty()) {
        autoStartProjectPath = startupProjectPath;
        autoStartBundlePath.clear();
        autoStartRequested = true;
        autoStartPlayerMode = false;
    }
#ifdef MODULARITY_PLAYER
    playerMode = true;
    autoStartPlayerMode = true;
#endif
    Profiler::instance().setRecording(std::getenv("MODULARITY_PROFILE_DUMP") != nullptr);
    if (playerMode) {
        showGameProfiler = false;
    }
    if (autoStartRequested && !autoStartBundlePath.empty() && autoStartBundleMode == "mount") {
        // data.win-style: read assets straight out of the single file. project.modu, scene files and
        // script binaries were shipped loose next to it, so the direct loaders still find them; the
        // fallback source resolves those from disk while the bundle serves everything else.
        const fs::path bundlePath = fs::path(autoStartBundlePath);
        const fs::path exportRoot = bundlePath.parent_path();
        std::string mountError;
        auto fallback = Modularity::Platform::MakeDesktopAssetSource(exportRoot.string());
        auto mounted = Modularity::Platform::MakeBundleAssetSource(
            std::move(fallback), bundlePath.string(), autoStartVerifyIntegrity, mountError);
        if (!mounted) {
            addConsoleMessage("Failed to mount packaged content: " + mountError,
                              ConsoleMessageType::Error);
            autoStartRequested = false;
            showLauncher = true;
        } else {
            Modularity::Platform::SetAssetSource(std::move(mounted));
            std::error_code cwdEc;
            fs::current_path(exportRoot, cwdEc);
            if (cwdEc) {
                addConsoleMessage("Failed to switch to packaged content root: " + cwdEc.message(),
                                  ConsoleMessageType::Error);
                autoStartRequested = false;
                showLauncher = true;
            } else {
                Modularity::Platform::SetPortableAssetRoot(exportRoot.string());
                fs::path projectPath = autoStartProjectPath.empty()
                    ? fs::path("project.modu")
                    : fs::path(autoStartProjectPath);
                autoStartProjectPath = (exportRoot / projectPath).lexically_normal().string();
            }
        }
    } else if (autoStartRequested && !autoStartBundlePath.empty()) {
        const fs::path bundlePath = fs::path(autoStartBundlePath);
        const fs::path runtimeCacheRoot = BuildRuntimeCacheRoot(projectManager.appDataPath, bundlePath);
        std::string bundleError;
        // First launch unpacks the whole bundle before ImGui or the renderer exist. Without pumping
        // events here a multi-GB bundle sits on a white window that Windows reports as Not
        // Responding, and players kill it mid-extract. Draw a bare progress bar with scissored
        // clears (no shaders needed) and let closing the window cancel.
        double lastProgressPump = -1.0;
        auto extractionProgress = [&](uint64_t bytesDone, uint64_t bytesTotal) -> bool {
            const double now = glfwGetTime();
            if (lastProgressPump >= 0.0 && now - lastProgressPump < 0.05) return true;
            lastProgressPump = now;
            glfwPollEvents();
            if (glfwWindowShouldClose(editorWindow)) return false;
            if (graphicsBackend != Modularity::GraphicsBackend::OpenGL) return true;

            int fbWidth = 0;
            int fbHeight = 0;
            glfwGetFramebufferSize(editorWindow, &fbWidth, &fbHeight);
            if (fbWidth <= 0 || fbHeight <= 0) return true;
            const float fraction = bytesTotal > 0
                ? std::clamp(static_cast<float>(static_cast<double>(bytesDone) / static_cast<double>(bytesTotal)), 0.0f, 1.0f)
                : 0.0f;
            const int barWidth = fbWidth * 3 / 5;
            const int barHeight = std::max(6, fbHeight / 90);
            const int barX = (fbWidth - barWidth) / 2;
            const int barY = fbHeight / 2 - barHeight / 2;

            glViewport(0, 0, fbWidth, fbHeight);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            glScissor(barX, barY, barWidth, barHeight);
            glClearColor(0.18f, 0.18f, 0.22f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            const int filled = static_cast<int>(static_cast<float>(barWidth) * fraction);
            if (filled > 0) {
                glScissor(barX, barY, filled, barHeight);
                glClearColor(0.62f, 0.52f, 0.95f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            glfwSwapBuffers(editorWindow);
            return true;
        };
        if (!ExtractRuntimeContentBundle(bundlePath, runtimeCacheRoot, bundleError, extractionProgress)) {
            addConsoleMessage("Failed to extract packaged runtime content: " + bundleError,
                              ConsoleMessageType::Error);
            autoStartRequested = false;
            showLauncher = true;
        } else {
            std::error_code cwdEc;
            fs::current_path(runtimeCacheRoot, cwdEc);
            if (cwdEc) {
                addConsoleMessage("Failed to switch runtime content root: " + cwdEc.message(),
                                  ConsoleMessageType::Error);
                autoStartRequested = false;
                showLauncher = true;
            } else {
#ifndef __ANDROID__
                // Opening the bundle created the desktop asset source, which roots relative lookups
                // at the cwd of that moment - the exe folder. Drop it so the next lookup recreates it
                // at the extracted content; otherwise Resources/Fonts and friends miss in every export.
                Modularity::Platform::SetAssetSource(nullptr);
#endif
                PruneRuntimeCache(projectManager.appDataPath, runtimeCacheRoot);
                fs::path projectPath = autoStartProjectPath.empty()
                    ? (runtimeCacheRoot / "project.modu")
                    : (runtimeCacheRoot / fs::path(autoStartProjectPath));
                autoStartProjectPath = projectPath.lexically_normal().string();
            }
        }
    }

    __initMark("window+config");
    {
        MODU_PROFILE_SCOPE("Engine::init setupImGui", ProfilerSampleCategory::Engine);
        setupImGui();
    }
    __initMark("setupImGui");

#if !MODULARITY_RUNTIME_ONLY
    applyGlobalEditorPreferences(true);
    applyEditorSoundTheme(projectManager.preferences.feedbackSoundTheme);
#endif

    if (usingVulkan()) {
        MODU_PROFILE_SCOPE("Engine::init initVulkanRenderer", ProfilerSampleCategory::Engine);
        if (!initVulkanRenderer()) {
            return false;
        }
    }

    {
        MODU_PROFILE_SCOPE("Engine::init audio", ProfilerSampleCategory::Audio);
        if (!audio.init()) {
            addConsoleMessage("Audio initialization failed. Audio playback will be disabled.", ConsoleMessageType::Warning);
        }
    }

    applyPresentationSettings();
    __initMark("audio+present");

    logToConsole("Engine initialized - Waiting for project selection");
    if (autoStartRequested && !autoStartProjectPath.empty()) {
        startProjectLoad(autoStartProjectPath);
    }
    return true;
}

bool Engine::initRenderer() {
    if (usingVulkan()) {
        return vulkanRendererInitialized;
    }
    if (rendererInitialized) return true;

    try {
        renderer.initialize();
        rendererInitialized = true;
        applySceneSkyboxSettings(sceneSkyboxSettings);
        applySceneTimeOfDay(sceneTimeOfDay);
        return true;
    } catch (const std::exception& e) {
        addConsoleMessage(std::string("OpenGL renderer initialization failed: ") + e.what(),
                          ConsoleMessageType::Error);
        std::cerr << "OpenGL renderer initialization failed: " << e.what() << std::endl;
        return false;
    } catch (...) {
        addConsoleMessage("OpenGL renderer initialization failed with an unknown error.",
                          ConsoleMessageType::Error);
        std::cerr << "OpenGL renderer initialization failed with an unknown error." << std::endl;
        return false;
    }
}

bool Engine::initVulkanRenderer() {
    if (!usingVulkan()) {
        return false;
    }
    if (vulkanRendererInitialized) {
        return true;
    }

#if !MODULARITY_HAS_VULKAN
    addConsoleMessage("Vulkan backend requested, but this build was compiled without Vulkan support.",
                      ConsoleMessageType::Error);
    return false;
#else
    vulkanRenderer = std::make_unique<Modularity::VulkanRenderer>();
    if (!vulkanRenderer->initialize(editorWindow)) {
        addConsoleMessage("Vulkan initialization failed: " + vulkanRenderer->getLastError(),
                          ConsoleMessageType::Error);
        vulkanRenderer.reset();
        return false;
    }
    if (!vulkanRenderer->initImGuiBackend()) {
        addConsoleMessage("Vulkan ImGui backend initialization failed: " + vulkanRenderer->getLastError(),
                          ConsoleMessageType::Error);
        vulkanRenderer->shutdown();
        vulkanRenderer.reset();
        return false;
    }

    vulkanRendererInitialized = true;
    applySceneSkyboxSettings(sceneSkyboxSettings);
    applySceneTimeOfDay(sceneTimeOfDay);
    addConsoleMessage("Initialized Vulkan backend (experimental).", ConsoleMessageType::Info);
    addConsoleMessage("Vulkan viewport preview supports built-in GLSL materials, albedo/overlay/normal textures, scene lights, and sky time-of-day.",
                      ConsoleMessageType::Warning);
    return true;
#endif
}

void Engine::logWindowLifecycle(const char* event, bool iconified, int fbWidth, int fbHeight) const {
    static const bool enabled = [] {
        const char* v = std::getenv("MODULARITY_WINDOW_LOG");
        return v && *v && std::string(v) != "0";
    }();
    if (!enabled) return;

    int winW = 0, winH = 0;
    if (editorWindow) {
        glfwGetWindowSize(editorWindow, &winW, &winH);
    }
    std::fprintf(stderr,
                 "[ModuWindow] %-14s iconified=%d client=%dx%d drawable=%dx%d "
                 "suspended=%d context=%s\n",
                 event, iconified ? 1 : 0, winW, winH, fbWidth, fbHeight,
                 renderSuspended ? 1 : 0,
                 glfwGetCurrentContext() ? "current" : "none");
    std::fflush(stderr);
}

#if !MODULARITY_RUNTIME_ONLY && !defined(__ANDROID__)
namespace {
// True when the user touched the editor this frame, or is still holding something down (a drag,
// a fly-camera key, a gizmo). InputEventsTrail holds exactly the events NewFrame() consumed, and
// ImGui drops duplicate mouse-position events, so a mouse resting over the window reads as idle.
bool EditorFrameHadUserInput() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return true;
    if (ctx->InputEventsTrail.Size > 0 || ImGui::IsAnyItemActive()) return true;
    for (bool down : ctx->IO.MouseDown) {
        if (down) return true;
    }
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key))) return true;
    }
    return false;
}
} // namespace
#endif

void Engine::run() {
    constexpr float kRigidbody2DFixedStep = 1.0f / 120.0f;
    constexpr int kMaxRigidbody2DStepsPerFrame = 4;
    float rigidbody2DAccumulator = 0.0f;
    bool runtime2DProfileWasEnabled = false;

    while (!glfwWindowShouldClose(editorWindow)) {
        const auto __frameWall = std::chrono::steady_clock::now();
        ++renderFrameSerial;
        renderer.setFrameSerial(renderFrameSerial);
        Modularity::DiscordPresence::Tick(projectManager.currentProject,
                                          playerMode || isPlaying,
                                          playerMode);

        {
            const double weatherNow = glfwGetTime();
            float weatherDt = (weatherLastTickTime > 0.0)
                                  ? static_cast<float>(weatherNow - weatherLastTickTime)
                                  : 0.0f;
            weatherLastTickTime = weatherNow;
            weatherDt = std::clamp(weatherDt, 0.0f, 0.25f);
            weatherClock += weatherDt * std::max(0.0f, sceneSkyboxSettings.weather.windSpeed);

            const WeatherDrive drive =
                EvaluateWeather(sceneSkyboxSettings.weather, weatherRuntime, weatherDt);
            renderer.setWeatherDrive(drive, weatherClock);
            if (Skybox* weatherSkybox = renderer.getSkybox()) {
                weatherSkybox->setWeatherDrive(drive);
            }

            if (drive.active && !drive.ambiencePath.empty()) {
                audio.playWeatherAmbience(drive.ambiencePath,
                                          sceneSkyboxSettings.weather.ambienceVolume);
            } else if (!audio.getWeatherAmbiencePath().empty()) {
                audio.stopWeatherAmbience();
            }
            if (drive.thunderTriggered &&
                !sceneSkyboxSettings.weather.thunderSoundPath.empty()) {
                audio.playOneShot(sceneSkyboxSettings.weather.thunderSoundPath,
                                  sceneSkyboxSettings.weather.ambienceVolume);
            }
        }
        Profiler& profiler = Profiler::instance();
        profiler.beginFrame(renderFrameSerial);
        static int gpuFrameTimerEnabled = -1;
        if (gpuFrameTimerEnabled < 0) {
            const char* v = std::getenv("MODULARITY_GPU_FRAME_TIMER");
            gpuFrameTimerEnabled = (v && *v == '0') ? 0 : 1;
            const char* rec = std::getenv("MODULARITY_PROFILER_RECORD");
            if (rec && *rec && *rec != '0') profiler.setRecording(true);
        }
        const bool gpuFrameQueryOpen =
            gpuFrameTimerEnabled ? profiler.beginOpenGlGpuFrame(true) : false;
        Modu2DStats::Reset();
        double frameStart = glfwGetTime();
        const auto frameStartClock = Runtime2DClock::now();
        const bool runtime2DProfileThisFrame =
            projectManager.currentProject.isLoaded &&
            !showLauncher &&
            isProject2DPipeline() &&
            (playerMode || isPlaying || specMode || testMode);
        gRuntime2DProfileEnabled = runtime2DProfileThisFrame;
        if (runtime2DProfileThisFrame) {
            if (!runtime2DProfileWasEnabled || gRuntime2DProfileAggregate.windowStartSec < 0.0) {
                resetRuntime2DAggregate(frameStart);
            }
            resetRuntime2DFrame();
            ModuRuntime2DRenderCounters_Reset();
        }
        runtime2DProfileWasEnabled = runtime2DProfileThisFrame;

        double profilePhysics2DMs = 0.0;
        double profileTransformMs = 0.0;
        double profileRenderSubmissionMs = 0.0;
        double profileActualDrawMs = 0.0;
        Runtime2DImGuiDrawCounters imguiDrawCounters;
        Runtime2DClock::time_point update2DStart;
        if (runtime2DProfileThisFrame) {
            update2DStart = Runtime2DClock::now();
        }

        {
            const bool iconified = glfwGetWindowAttrib(editorWindow, GLFW_ICONIFIED) != 0;
            int suspendFbW = 0, suspendFbH = 0;
            glfwGetFramebufferSize(editorWindow, &suspendFbW, &suspendFbH);
            const bool zeroSizedDrawable = (suspendFbW <= 0 || suspendFbH <= 0);

            if (iconified || zeroSizedDrawable) {
                if (!renderSuspended) {
                    renderSuspended = true;
                    logWindowLifecycle("suspend", iconified, suspendFbW, suspendFbH);
                }
                glfwPollEvents();
                ImGui_ImplGlfw_Sleep(10);
                lastFrame = glfwGetTime();
                profiler.endFrame();
                continue;
            }

            if (renderSuspended) {
                renderSuspended = false;
                glfwMakeContextCurrent(editorWindow);
                lastFrame = glfwGetTime();
                renderResumeLogPending = true;
                logWindowLifecycle("restore", iconified, suspendFbW, suspendFbH);
            }
        }

        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        deltaTime = std::min(deltaTime, 1.0f / 30.0f);
        gameTimeScale = std::clamp(gameTimeScale, kMinGameTimeScale, kMaxGameTimeScale);
        gameDeltaTime = deltaTime * gameTimeScale;

        const auto __preStart = std::chrono::steady_clock::now();
        {
            MODU_PROFILE_SCOPE("Poll Events", ProfilerSampleCategory::Engine);
            glfwPollEvents();
        }
#ifdef __ANDROID__
        if (!Modularity::AndroidRuntime::PollEvents()) {
            glfwSetWindowShouldClose(editorWindow, GLFW_TRUE);
            break;
        }
        if (!Modularity::AndroidRuntime::HasRenderSurface()) {
            continue;
        }
#endif
        pollProjectLoad();
        pollSceneLoad();

        updateXRSystem();
#if !MODULARITY_RUNTIME_ONLY
        if (!playerMode) updateVREditorShortcut();
#endif
        updateXRComponents(deltaTime);

        if (autoPlayOnLoad_ && !isPlaying && !showLauncher &&
            projectManager.currentProject.isLoaded && !sceneLoadInProgress &&
            !sceneObjects.empty()) {
            autoPlayOnLoad_ = false;
            applyAutoStartMode();
            gameViewCursorLocked = false;
            if (editorWindow) {
                glfwSetInputMode(editorWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }

#if MODULARITY_RUNTIME_ONLY
        const bool termsPending = false;
#else
        const bool termsPending = requiresTermsOfServiceAcceptance();
#endif

        if (!showLauncher && !termsPending) {
            handleKeyboardShortcuts();
        }

        if (termsPending) {
            cursorLocked = false;
            gameViewCursorLocked = false;
            viewportController.setFocused(false);
        }

        if (gameViewCursorLocked) {
            cursorLocked = false;
            viewportController.setFocused(false);
        }
        viewportController.update(editorWindow, cursorLocked);
        if (!isPlaying) {
            gameViewCursorLocked = false;
        }

        if (viewportController.isViewportFocused() && cursorLocked) {
            ImGuiIO& io = ImGui::GetIO();
            if (std::abs(io.MouseDelta.x) > 0.0001f || std::abs(io.MouseDelta.y) > 0.0001f) {
                camera.processMouseDelta(io.MouseDelta.x, -io.MouseDelta.y);
            }
        }

        if (viewportController.isViewportFocused() && cursorLocked) {
            camera.processKeyboard(deltaTime, editorWindow);
        } else {
            camera.velocity = glm::vec3(0.0f);
        }

        if (viewportFocusActive) {
            if (cursorLocked) {
                viewportFocusActive = false;
            } else {
                const double elapsed = currentFrame - viewportFocusStartTime;
                const float t = static_cast<float>(std::clamp(
                    elapsed / std::max(0.001, viewportFocusDuration), 0.0, 1.0));
                const float eased = t * t * (3.0f - 2.0f * t);
                camera.position = glm::mix(viewportFocusStartPosition,
                                           viewportFocusTargetPosition,
                                           eased);
                glm::vec3 front = glm::mix(viewportFocusStartFront,
                                           viewportFocusTargetFront,
                                           eased);
                if (std::isfinite(front.x) && glm::length(front) > 1e-4f) {
                    camera.front = glm::normalize(front);
                    camera.pitch = glm::degrees(std::asin(glm::clamp(camera.front.y, -1.0f, 1.0f)));
                    camera.pitch = glm::clamp(camera.pitch, -89.0f, 89.0f);
                    camera.yaw = glm::degrees(std::atan2(camera.front.z, camera.front.x));
                    camera.firstMouse = true;
                }
                if (t >= 1.0f) {
                    viewportFocusActive = false;
                }
            }
        }

        // Entering or leaving play restarts every Particle System, so Play On Awake means what
        // it says and an editor preview does not carry into the game. Before the scripts run,
        // so a Particles.Play from a Begin on the first play frame is not undone straight after.
        if (isPlaying != particlesLastPlaying) {
            particlesLastPlaying = isPlaying;
            for (SceneObject& obj : sceneObjects) {
                if (obj.hasParticleSystem2D) ParticleRuntime::ResetRuntime(obj.particleSystem2D);
            }
        }

        // Run scripts only in play/spec/test modes to avoid edit-time side effects (e.g., cursor grabs)
        if (projectManager.currentProject.isLoaded) {
            bool runScripts = isPlaying || specMode || testMode;
            if (runScripts) {
                updateScripts(gameDeltaTime);
            }
            updateNetworking(deltaTime);
        }

        // Scripts consume the previous frame's UI events. Clear transient state
        // once before rendering, including controls that are now hidden or clipped.
        for (SceneObject& obj : sceneObjects) {
            if (!obj.hasUI || !IsExtendedUIType(obj.ui.type)) continue;
            obj.ui.buttonPressed = obj.ui.inputSubmitted = obj.ui.valueChanged = false;
            obj.ui.uiHovered = obj.ui.uiActive = obj.ui.uiFocused = false;
        }

        if (isPlaying) {
            MODU_PROFILE_SCOPE("Player Controller", ProfilerSampleCategory::Engine);
            updatePlayerController(gameDeltaTime);
        }

        bool simulate2D = (isPlaying && !isPaused) || (!isPlaying && specMode) || (!isPlaying && testMode);
        if (simulate2D) {
            MODU_PROFILE_SCOPE("Physics 2D", ProfilerSampleCategory::Physics);
            rigidbody2DAccumulator = std::min(
                rigidbody2DAccumulator + gameDeltaTime,
                kRigidbody2DFixedStep * static_cast<float>(kMaxRigidbody2DStepsPerFrame));

            int simulateSteps = 0;
            while (rigidbody2DAccumulator >= kRigidbody2DFixedStep &&
                   simulateSteps < kMaxRigidbody2DStepsPerFrame) {
                Runtime2DClock::time_point physicsStepStart;
                if (runtime2DProfileThisFrame) {
                    physicsStepStart = Runtime2DClock::now();
                }
                updateRigidbody2D(kRigidbody2DFixedStep);
                if (runtime2DProfileThisFrame) {
                    profilePhysics2DMs += Runtime2DMsSince(physicsStepStart, Runtime2DClock::now());
                }
                rigidbody2DAccumulator -= kRigidbody2DFixedStep;
                ++simulateSteps;
            }
        } else {
            rigidbody2DAccumulator = 0.0f;
        }

        if (isPlaying) {
            MODU_PROFILE_SCOPE("Camera Follow", ProfilerSampleCategory::Engine);
            updateCameraFollow2D(gameDeltaTime);
        }

        float runtimeAnimDelta = ((isPlaying && isPaused) ? 0.0f : gameDeltaTime);
        {
            MODU_PROFILE_SCOPE("Animation", ProfilerSampleCategory::Animation);
            syncVideoPlayers(runtimeAnimDelta);
            updateRuntimeAnimations(runtimeAnimDelta);
            updateSkeletalAnimations(runtimeAnimDelta);
        }
        if (runtime2DProfileThisFrame) {
            const auto transformStart = Runtime2DClock::now();
            MODU_PROFILE_SCOPE("Hierarchy Transforms", ProfilerSampleCategory::Engine);
            updateHierarchyWorldTransforms();
            profileTransformMs += Runtime2DMsSince(transformStart, Runtime2DClock::now());
        } else {
            MODU_PROFILE_SCOPE("Hierarchy Transforms", ProfilerSampleCategory::Engine);
            updateHierarchyWorldTransforms();
        }

        bool hasRuntime3DPhysics = false;
        bool hasRuntimeAIAgent = false;
        bool hasActiveSkeletal = false;
        const bool runtimeSystemsActive = (isPlaying || specMode || testMode);
        for (const SceneObject& obj : sceneObjects) {
            if (!IsObjectEnabledInHierarchy(obj)) continue;
            if (runtimeSystemsActive &&
                !hasRuntime3DPhysics &&
                ((obj.hasRigidbody && obj.rigidbody.enabled) ||
                 (obj.hasCollider && obj.collider.enabled))) {
                hasRuntime3DPhysics = true;
            }
            if (runtimeSystemsActive && !hasRuntimeAIAgent && obj.hasAIAgent) {
                hasRuntimeAIAgent = true;
            }
            if (!hasActiveSkeletal &&
                obj.hasSkeletalAnimation &&
                obj.skeletal.enabled &&
                !obj.skeletal.finalMatrices.empty()) {
                hasActiveSkeletal = true;
            }
            if ((!runtimeSystemsActive || (hasRuntime3DPhysics && hasRuntimeAIAgent)) && hasActiveSkeletal) {
                break;
            }
        }

        bool simulatePhysics = physics->isReady() &&
                               ((isPlaying && !isPaused) || (!isPlaying && specMode)) &&
                               hasRuntime3DPhysics;
        if (simulatePhysics) {
            MODU_PROFILE_SCOPE("Physics 3D", ProfilerSampleCategory::Physics);
            physics->simulate(gameDeltaTime, sceneObjects);
            dispatchPhysicsCollisionEvents(gameDeltaTime);
        }
        bool runAI = (isPlaying || specMode || testMode) && hasRuntimeAIAgent;
        if (runAI) {
            MODU_PROFILE_SCOPE("AI", ProfilerSampleCategory::AI);
            updateAIAgents(gameDeltaTime);
        }

        if (simulatePhysics || runAI) {
            MODU_PROFILE_SCOPE("Hierarchy Transforms (post-physics)", ProfilerSampleCategory::Engine);
            const auto postPhysicsTransformStart = Runtime2DClock::now();
            updateHierarchyWorldTransforms();
            if (runtime2DProfileThisFrame) {
                profileTransformMs +=
                    Runtime2DMsSince(postPhysicsTransformStart, Runtime2DClock::now());
            }
        }

        if (hasActiveSkeletal) {
            MODU_PROFILE_SCOPE("Skinning", ProfilerSampleCategory::Animation);
            updateSkinningMatrices();
        }
        {
            MODU_PROFILE_SCOPE("Particles", ProfilerSampleCategory::Engine);
            updateParticleSystems(runtimeAnimDelta);
        }
        if (runtime2DProfileThisFrame) {
            gRuntime2DProfileFrame.update2DTotalMs += Runtime2DMsSince(update2DStart, Runtime2DClock::now());
            gRuntime2DProfileFrame.physics2DTotalMs += profilePhysics2DMs;
            gRuntime2DProfileFrame.transformUpdateMs += profileTransformMs;
        }

        if (playerMode) {
            syncPlayerCamera();
        }

        const SceneObject* runtimeCameraObject = findPlayerCameraObject();

        if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
            if (!showLauncher && projectManager.currentProject.isLoaded) {
                vulkanRenderer->setSkyboxTimeOfDay(getSceneTimeOfDay());

                if (!vulkanMaterialFeatureWarningShown) {
                    const bool hasUnsupportedVulkanMaterials = std::any_of(
                        sceneObjects.begin(), sceneObjects.end(),
                        [](const SceneObject& obj) { return hasUnsupportedVulkanPreviewFeature(obj); });
                    if (hasUnsupportedVulkanMaterials) {
                        addConsoleMessage(
                            "Vulkan preview fallback is active for some scene features. "
                            "Unsupported in Vulkan preview: custom shader pairs and non-cube primitive/mesh geometry.",
                            ConsoleMessageType::Warning);
                        vulkanMaterialFeatureWarningShown = true;
                    }
                }

                vulkanRenderer->setViewportSceneData(
                    sceneObjects,
                    camera,
                    buildSettings.editorCameraFov,
                    buildSettings.editorCameraNear,
                    buildSettings.editorCameraFar);

                if (runtimeCameraObject) {
                    const SceneObject* runtimeCam = runtimeCameraObject;
                    Camera gameCamera = makeCameraFromObject(*runtimeCam);
                    int vkRenderWidth = kRuntimeInternalWidth;
                    int vkRenderHeight = kRuntimeInternalHeight;
                    getRuntimeInternalResolution(vkRenderWidth, vkRenderHeight);
                    vulkanRenderer->setGameSceneData(
                        sceneObjects,
                        &gameCamera,
                        ResolveCameraVerticalFovDeg(runtimeCam->camera,
                                                    static_cast<float>(vkRenderWidth) /
                                                        static_cast<float>(std::max(1, vkRenderHeight))),
                        runtimeCam->camera.nearClip,
                        runtimeCam->camera.farClip);
                } else {
                    vulkanRenderer->clearGameSceneData();
                }
            } else {
                vulkanRenderer->clearViewportSceneData();
                vulkanRenderer->clearGameSceneData();
            }
        }

        bool audioShouldPlay = isPlaying || specMode || testMode;
        Camera listenerCamera = camera;
        if (runtimeCameraObject) {
            const SceneObject* runtimeCam = runtimeCameraObject;
            listenerCamera = makeCameraFromObject(*runtimeCam);
            listenerCamera.position = runtimeCam->position;
        }
        audio.setPrefer2DSpatialAudio(isProject2DPipeline() || uiWorldMode);
        {
            MODU_PROFILE_SCOPE("Audio", ProfilerSampleCategory::Audio);
            const auto __auStart = std::chrono::steady_clock::now();
            audio.update(sceneObjects, listenerCamera, audioShouldPlay);
            const double __auMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - __auStart).count();
            if (__auMs > Modularity::HardwareProfile::TimingBudget(50.0)) std::fprintf(stderr, "[ModuTimer]   audio.update %.1f ms\n", __auMs);
        }

        if (!playerMode) {
            MODU_PROFILE_SCOPE("Asset Loading", ProfilerSampleCategory::Asset);
            auto __t = std::chrono::steady_clock::now();
            auto __mark = [&](const char* name) {
                const auto now = std::chrono::steady_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(now - __t).count();
                if (ms > Modularity::HardwareProfile::TimingBudget(50.0)) std::fprintf(stderr, "[ModuTimer]   %s %.1f ms\n", name, ms);
                __t = now;
            };
            updateCompileJob();           __mark("updateCompileJob");
            updateScriptReloadRequests(); __mark("updateScriptReloadRequests");
            updateAutoCompileScripts();   __mark("updateAutoCompileScripts");
            processAutoCompileQueue();    __mark("processAutoCompileQueue");
            pollExportBuild();            __mark("pollExportBuild");
        }

        if (playerMode && !showLauncher) {
            int displayW = 0;
            int displayH = 0;
#ifdef __ANDROID__
            Modularity::AndroidRuntime::GetSurfaceSize(&displayW, &displayH);
#else
            glfwGetFramebufferSize(editorWindow, &displayW, &displayH);
#endif
            if (displayW > 0 && displayH > 0) {
                int runtimeRenderWidth = kRuntimeInternalWidth;
                int runtimeRenderHeight = kRuntimeInternalHeight;
                getRuntimeInternalResolution(runtimeRenderWidth, runtimeRenderHeight);
                viewportWidth = runtimeRenderWidth;
                viewportHeight = runtimeRenderHeight;
                if (rendererInitialized) {
                    renderer.resize(viewportWidth, viewportHeight);
                }
            }
        }

        if (playerMode && !showLauncher && projectManager.currentProject.isLoaded && rendererInitialized) {
            MODU_PROFILE_SCOPE("Render", ProfilerSampleCategory::Render);
            Runtime2DClock::time_point renderSubmissionStart;
            if (runtime2DProfileThisFrame) {
                renderSubmissionStart = Runtime2DClock::now();
            }
            int runtimeRenderWidth = kRuntimeInternalWidth;
            int runtimeRenderHeight = kRuntimeInternalHeight;
            getRuntimeInternalResolution(runtimeRenderWidth, runtimeRenderHeight);
            float renderFov = buildSettings.editorCameraFov;
            float renderNear = buildSettings.editorCameraNear;
            float renderFar = buildSettings.editorCameraFar;
            Camera activeRenderCamera = camera;
            if (playerMode) {
                if (runtimeCameraObject) {
                    const SceneObject* runtimeCam = runtimeCameraObject;
                    activeRenderCamera = makeCameraFromObject(*runtimeCam);
                    renderFov = ResolveCameraVerticalFovDeg(
                        runtimeCam->camera,
                        static_cast<float>(runtimeRenderWidth) /
                            static_cast<float>(std::max(1, runtimeRenderHeight)));
                    renderNear = std::max(0.01f, runtimeCam->camera.nearClip);
                    renderFar = std::max(renderNear + 0.01f, runtimeCam->camera.farClip);
                }
            }
            const bool ownsGpuQuery = profiler.beginOpenGlGpuFrame();
            const bool pure2DRender =
                isProject2DPipeline() ||
                (runtimeCameraObject && runtimeCameraObject->camera.use2D);
            xrFrameSubmitted = xrSystem.isRunning() && renderXRFrame();
            if (xrFrameSubmitted) {
            } else if (isProject25DPipeline()) {
                renderTMViewportPass(activeRenderCamera,
                                     runtimeRenderWidth,
                                     runtimeRenderHeight,
                                     renderFov,
                                     renderNear,
                                     renderFar);
            } else if (!pure2DRender) {
                glm::mat4 view = activeRenderCamera.getViewMatrix();
                glm::mat4 proj = glm::perspective(glm::radians(renderFov),
                                                  static_cast<float>(runtimeRenderWidth) /
                                                      static_cast<float>(std::max(1, runtimeRenderHeight)),
                                                  renderNear,
                                                  renderFar);
                const auto __rsStart = std::chrono::steady_clock::now();
                renderer.beginRender(view, proj, activeRenderCamera.position);
                renderer.renderScene(activeRenderCamera, sceneObjects, selectedObjectId,
                                     renderFov,
                                     renderNear,
                                     renderFar,
                                     false,
                                     &selectedObjectIds,
                                     SceneRenderMode::Normal,
                                     !playerMode);
                renderer.endRender();
                const double __rsMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - __rsStart).count();
                if (__rsMs > Modularity::HardwareProfile::TimingBudget(100.0)) std::fprintf(stderr, "[ModuTimer] renderScene(editor) %.1f ms\n", __rsMs);
            }
            if (ownsGpuQuery) {
                profiler.endOpenGlGpuFrame();
            }
            if (runtime2DProfileThisFrame) {
                profileRenderSubmissionMs += Runtime2DMsSince(renderSubmissionStart, Runtime2DClock::now());
            }
        }

#if !MODULARITY_RUNTIME_ONLY
        if (!playerMode && isPlaying && !vrEditor.requested) {
            xrFrameSubmitted = xrSystem.isRunning() && renderXRFrame();
        }
#endif

        {
            MODU_PROFILE_SCOPE("Editor UI", ProfilerSampleCategory::UI);
            if (usingVulkan()) {
#if MODULARITY_HAS_VULKAN
                if (vulkanRendererInitialized &&
                    vulkanRenderer &&
                    projectManager.currentProject.isLoaded &&
                    !showLauncher) {
                    if (!vulkanRenderer->prepareFrameResources()) {
                        static bool loggedVulkanPrepareError = false;
                        if (!loggedVulkanPrepareError) {
                            addConsoleMessage("Vulkan scene target preparation failed: " + vulkanRenderer->getLastError(),
                                              ConsoleMessageType::Error);
                            loggedVulkanPrepareError = true;
                        }
                    }
                }
                ImGui_ImplVulkan_NewFrame();
#endif
            } else {
                ImGui_ImplOpenGL3_NewFrame();
            }
            ImGui_ImplGlfw_NewFrame();
#ifdef __ANDROID__
            {
                int eglW = 0, eglH = 0;
                Modularity::AndroidRuntime::GetSurfaceSize(&eglW, &eglH);
                if (eglW > 0 && eglH > 0) {
                    ImGuiIO& __io = ImGui::GetIO();
                    __io.DisplaySize = ImVec2(static_cast<float>(eglW),
                                              static_cast<float>(eglH));
                    __io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                    __io.DeltaTime = std::max(0.0001f, deltaTime);
                }
            }
            {
                float px = 0.0f, py = 0.0f;
                bool active = false;
                Modularity::AndroidRuntime::GetPrimaryPointer(&px, &py, &active);
                ImGuiIO& __io = ImGui::GetIO();
                __io.AddMousePosEvent(px, py);
                const bool __feedBtn = updateAndroidTouchScroll(px, py, active)
                                           ? active
                                           : false;
                __io.AddMouseButtonEvent(0, __feedBtn);

                auto __akToImGui = [](int kc) -> ImGuiKey {
                    switch (kc) {
                        case AKEYCODE_DEL:           return ImGuiKey_Backspace;
                        case AKEYCODE_FORWARD_DEL:   return ImGuiKey_Delete;
                        case AKEYCODE_ENTER:
                        case AKEYCODE_NUMPAD_ENTER:  return ImGuiKey_Enter;
                        case AKEYCODE_TAB:           return ImGuiKey_Tab;
                        case AKEYCODE_ESCAPE:        return ImGuiKey_Escape;
                        case AKEYCODE_DPAD_LEFT:     return ImGuiKey_LeftArrow;
                        case AKEYCODE_DPAD_RIGHT:    return ImGuiKey_RightArrow;
                        case AKEYCODE_DPAD_UP:       return ImGuiKey_UpArrow;
                        case AKEYCODE_DPAD_DOWN:     return ImGuiKey_DownArrow;
                        case AKEYCODE_MOVE_HOME:     return ImGuiKey_Home;
                        case AKEYCODE_MOVE_END:      return ImGuiKey_End;
                        case AKEYCODE_SHIFT_LEFT:    return ImGuiKey_LeftShift;
                        case AKEYCODE_SHIFT_RIGHT:   return ImGuiKey_RightShift;
                        case AKEYCODE_SPACE:         return ImGuiKey_Space;
                        default:                     return ImGuiKey_None;
                    }
                };
                unsigned int __ch = 0;
                while (Modularity::AndroidRuntime::PollInputChar(&__ch)) {
                    __io.AddInputCharacter(__ch);
                }
                int __kc = 0;
                bool __kdown = false;
                while (Modularity::AndroidRuntime::PollKeyEvent(&__kc, &__kdown)) {
                    const ImGuiKey __key = __akToImGui(__kc);
                    if (__key != ImGuiKey_None) __io.AddKeyEvent(__key, __kdown);
                }
                Modularity::AndroidRuntime::SetSoftKeyboardVisible(__io.WantTextInput);
            }
#endif
            ImGui::NewFrame();
            overlayPostFxRequests.clear();
            const double __preMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - __preStart).count();
            if (__preMs > Modularity::HardwareProfile::TimingBudget(100.0)) std::fprintf(stderr, "[ModuTimer] preImGuiWork %.1f ms\n", __preMs);
            [[maybe_unused]] const auto __dispatchStart = std::chrono::steady_clock::now();
            uiCanvas3DInputs.clear();
#if !MODULARITY_RUNTIME_ONLY
            // Runs on the launcher too, so Clone from Git can finish there.
            if (!playerMode) updateVersionControl();
#endif

#if !MODULARITY_RUNTIME_ONLY
            if (!playerMode && isVREditorActive()) {
                renderVREditorMonitor();
            } else
#endif
            if (showLauncher) {
                mainDockspaceId = 0;
                #ifdef MODULARITY_PLAYER
                renderPlayerViewport();
                #else
                renderLauncher();
                #endif
            } else if (!playerMode) {
#if !MODULARITY_RUNTIME_ONLY
                refreshEditorWindowIcons();
                const auto __dsStart = std::chrono::steady_clock::now();
                mainDockspaceId = setupDockspace(uiChromeScale);
                const double __dsMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - __dsStart).count();
                if (__dsMs > Modularity::HardwareProfile::TimingBudget(50.0)) std::fprintf(stderr, "[ModuTimer] setupDockspace %.1f ms\n", __dsMs);
                const auto __mbStart = std::chrono::steady_clock::now();
                renderMainMenuBar();
                const double __mbMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - __mbStart).count();
                if (__mbMs > Modularity::HardwareProfile::TimingBudget(50.0)) std::fprintf(stderr, "[ModuTimer] mainMenuBar %.1f ms\n", __mbMs);
                const bool skipDockedWindowsThisFrame = workspaceLayoutSettlingFrame;
                workspaceLayoutSettlingFrame = false;

#define MODU_TIME_PANEL(name, expr) do { \
    const auto __pStart = std::chrono::steady_clock::now(); \
    expr; \
    const double __pMs = std::chrono::duration<double, std::milli>( \
        std::chrono::steady_clock::now() - __pStart).count(); \
    Profiler::instance().addSyntheticSample("panel:" name, ProfilerSampleCategory::UI, __pMs); \
    if (__pMs > Modularity::HardwareProfile::TimingBudget(100.0)) std::fprintf(stderr, "[ModuTimer] panel:" name " %.1f ms\n", __pMs); \
} while(0)
                fileBrowserPanelFocused = false;
                if (!viewportFullscreen && !skipDockedWindowsThisFrame) {
                    if (showHierarchy) MODU_TIME_PANEL("Hierarchy", renderHierarchyPanel());
                    if (showInspector) MODU_TIME_PANEL("Inspector", renderInspectorPanel());
                    if (showFileBrowser) MODU_TIME_PANEL("FileBrowser", renderFileBrowserPanel());
                    if (showMeshBuilder && hasMeshBuilderPackage()) MODU_TIME_PANEL("MeshBuilder", renderMeshBuilderPanel());
                    if (showScriptingWindow && hasScriptingWindowPackage()) MODU_TIME_PANEL("Scripting", renderScriptingWindow());
                    if (showEnvironmentWindow) MODU_TIME_PANEL("Environment", renderEnvironmentWindow());
                    if (showCameraWindow) MODU_TIME_PANEL("Camera", renderCameraWindow());
                    if (showAnimationWindow) MODU_TIME_PANEL("Animation", renderAnimationWindow());
                    if (showAIPathfindingWindow) MODU_TIME_PANEL("AIPath", renderAIPathfindingWindow());
                    if (showPixelSpriteEditorWindow && hasSpriteEditorPackage()) MODU_TIME_PANEL("PixelSprite", renderPixelSpriteEditorWindow());
                    if (showVisualScriptingWindow) MODU_TIME_PANEL("VisualScript", renderVisualScriptingWindow());
                    if (showSectorMapWindow) MODU_TIME_PANEL("SectorMap", renderSectorMapWindow());
#if !MODULARITY_RUNTIME_ONLY
                    if (showAssemblageWindow) MODU_TIME_PANEL("Assemblage", renderAssemblageWindow());
#endif
                    if (showLightmappingWindow) MODU_TIME_PANEL("Lightmapping", renderLightmappingWindow());
                    if (showXRPreviewWindow) MODU_TIME_PANEL("Stereo Preview", renderXRPreviewWindow());
                    if (showProjectBrowser) MODU_TIME_PANEL("ProjectBrowser", renderProjectBrowserPanel());
                    if (showGameProfilerWindow) MODU_TIME_PANEL("Profiler", renderGameProfilerWindow());
                    if (showModularityDoctorWindow) MODU_TIME_PANEL("Doctor", renderModularityDoctorWindow());
#if !MODULARITY_RUNTIME_ONLY
                    if (showVersionControlWindow) MODU_TIME_PANEL("VersionControl", renderVersionControlWindow());
#endif
                    if (showRegistryPackagesWindow) MODU_TIME_PANEL("Registry", renderRegistryPackagesWindow());
                }

                if (showBuildSettings) MODU_TIME_PANEL("BuildSettings", renderBuildSettingsWindow());
                if (!skipDockedWindowsThisFrame) {
                    MODU_TIME_PANEL("ScriptEditorWindows", renderScriptEditorWindows());
                    if (showViewport) MODU_TIME_PANEL("Viewport", renderViewport());
                    if (showGameViewport) MODU_TIME_PANEL("GameViewport", renderGameViewportWindow());
                    if (showConsole) MODU_TIME_PANEL("Console", renderConsolePanel());
                }
#undef MODU_TIME_PANEL
                updateLightmapBake();
                renderLatestErrorBar();
                renderEditorToast();
                renderTouchEditToolbar();
                updateDockDrawerInteractions();
                {
                    const double __dispatchMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - __dispatchStart).count();
                    if (__dispatchMs > Modularity::HardwareProfile::TimingBudget(100.0)) std::fprintf(stderr, "[ModuTimer] panelDispatch %.1f ms\n", __dispatchMs);
                }
#else
                mainDockspaceId = 0;
                renderPlayerViewport();
#endif
            } else {
                mainDockspaceId = 0;
                renderPlayerViewport();
            }

            if (!playerMode) {
#if !MODULARITY_RUNTIME_ONLY
                if (!isVREditorActive()) {
                renderTermsOfServiceModal();
                renderWindowsDisclaimerPopup();
                renderLowSpecBenchmarkPopup();
                renderAndroidStorageAccessPopup();
                renderModuCppNvimWarningPopup();
                renderEditorLanguagePromptPopup();
                renderDialogs();
                renderModuPakExportDialog();
                renderModuPakImportDialog();
                renderModuObjExportDialog();
                renderModuObjImportDialog();
                renderModuObjEditorBar();
                }
#endif
            }
        }

        const auto __postStart = std::chrono::steady_clock::now();
        if (!playerMode) {
            updateTouchSwipeScrolling();
            autosaveWorkspaceLayout();
        }
        const double __postMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - __postStart).count();
        if (__postMs > Modularity::HardwareProfile::TimingBudget(50.0)) std::fprintf(stderr, "[ModuTimer] postPanels(touch+autosave) %.1f ms\n", __postMs);
        const auto __uiC3DStart = std::chrono::steady_clock::now();
        renderUiCanvas3DTargets();
#if !MODULARITY_RUNTIME_ONLY
        if (!playerMode) {
            DragPreview::UpdateAndRender();
            flushWidgetFeedbackSounds();
        }
#endif
        const auto __renderImGuiStart = std::chrono::steady_clock::now();
        {
            MODU_PROFILE_SCOPE("ImGui Render", ProfilerSampleCategory::UI);
            ImGui::Render();
        }
#if !MODULARITY_RUNTIME_ONLY
        if (!playerMode && vrEditor.requested) {
            if (xrSystem.isRunning()) xrFrameSubmitted = renderXRFrame();
            else if (vrEditor.preview) {
                const auto previewInput=vrEditor.previewInputState();
                renderVREditorUI(previewInput);
                const bool previewScene=rendererInitialized && projectManager.currentProject.isLoaded && !showLauncher && !sceneLoadInProgress;
                vrEditor.renderPreview(renderer,previewScene?&camera:nullptr,previewScene?&sceneObjects:nullptr,
                                       buildSettings.editorCameraNear,buildSettings.editorCameraFar);
                applyVREditorActions();
            } else applyVREditorActions(); // The monitor remains usable while XR is starting.
        }
#endif
        const auto __renderImGuiEnd = std::chrono::steady_clock::now();
        {
            const double __c3DMs = std::chrono::duration<double, std::milli>(__renderImGuiStart - __uiC3DStart).count();
            const double __irMs = std::chrono::duration<double, std::milli>(__renderImGuiEnd - __renderImGuiStart).count();
            if (__c3DMs > Modularity::HardwareProfile::TimingBudget(50.0)) std::fprintf(stderr, "[ModuTimer] uiCanvas3D %.1f ms\n", __c3DMs);
            if (__irMs > Modularity::HardwareProfile::TimingBudget(50.0)) std::fprintf(stderr, "[ModuTimer] ImGui::Render %.1f ms\n", __irMs);
        }
        if (runtime2DProfileThisFrame) {
            imguiDrawCounters = CollectImGuiDrawCounters(ImGui::GetDrawData());
        }

        Runtime2DClock::time_point drawRenderStart;
        if (runtime2DProfileThisFrame) {
            drawRenderStart = Runtime2DClock::now();
        }

        if (usingVulkan()) {
            if (vulkanRendererInitialized && vulkanRenderer) {
                const ImVec4 clearColor(0.1f, 0.1f, 0.12f, 1.0f);
                if (!vulkanRenderer->renderFrame(ImGui::GetDrawData(), clearColor)) {
                    static bool loggedVulkanFrameError = false;
                    if (!loggedVulkanFrameError) {
                        addConsoleMessage("Vulkan frame submission failed: " + vulkanRenderer->getLastError(),
                                          ConsoleMessageType::Error);
                        loggedVulkanFrameError = true;
                    }
                }
            }
        } else {
            int displayW = 0;
            int displayH = 0;
#ifdef __ANDROID__
            Modularity::AndroidRuntime::GetSurfaceSize(&displayW, &displayH);
#else
            glfwGetFramebufferSize(editorWindow, &displayW, &displayH);
#endif
            // Desktop UI always belongs to the window, never an offscreen/XR eye target.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, displayW, displayH);
            glClearColor(0.1f, 0.1f, 0.12f, 1.00f);
            glClear(GL_COLOR_BUFFER_BIT);

            const auto __glDrawStart = std::chrono::steady_clock::now();
            {
                MODU_PROFILE_SCOPE("ImGui Draw Submit", ProfilerSampleCategory::Render);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            }
            if (rendererInitialized && renderer.isBandPostFxRegionActive()) {
                renderer.endBandPostFxRegion(Camera{}, sceneObjects, false);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, displayW, displayH);
            }
            const double __glDrawMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - __glDrawStart).count();
            if (__glDrawMs > Modularity::HardwareProfile::TimingBudget(50.0)) std::fprintf(stderr, "[ModuTimer] OpenGL3_RenderDrawData %.1f ms\n", __glDrawMs);

            if (gpuFrameQueryOpen) {
                profiler.endOpenGlGpuFrame();
            }

            ImGuiIO& io = ImGui::GetIO();
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                const ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
                if (platformIo.Viewports.Size > 1) {
                    GLFWwindow* backup_current_context = glfwGetCurrentContext();
                    ImGui::UpdatePlatformWindows();
                    ImGui::RenderPlatformWindowsDefault();
                    glfwMakeContextCurrent(backup_current_context);
                }
            }

            if (materialColorSamplerActive) {
                if (materialColorSamplerAwaitMouseRelease) {
                    if (!io.MouseDown[ImGuiMouseButton_Left]) {
                        materialColorSamplerAwaitMouseRelease = false;
                    }
                } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                           ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    materialColorSamplerActive = false;
                    materialColorSamplerTargetId.clear();
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                           displayW > 0 && displayH > 0 &&
                           io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
                    const float scaleX = static_cast<float>(displayW) / io.DisplaySize.x;
                    const float scaleY = static_cast<float>(displayH) / io.DisplaySize.y;
                    const int sampleX = std::clamp(
                        static_cast<int>(std::floor(io.MousePos.x * scaleX)),
                        0,
                        displayW - 1
                    );
                    const int sampleY = std::clamp(
                        displayH - 1 - static_cast<int>(std::floor(io.MousePos.y * scaleY)),
                        0,
                        displayH - 1
                    );

                    GLint previousReadFramebuffer = 0;
                    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
#if !MODULARITY_OPENGL_ES
                    glReadBuffer(GL_BACK);
#endif

                    std::array<unsigned char, 4> pixel = {0, 0, 0, 255};
                    glReadPixels(sampleX, sampleY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));

                    materialColorSamplerResult = glm::vec4(
                        static_cast<float>(pixel[0]) / 255.0f,
                        static_cast<float>(pixel[1]) / 255.0f,
                        static_cast<float>(pixel[2]) / 255.0f,
                        1.0f
                    );
                    materialColorSamplerHasResult = true;
                    materialColorSamplerActive = false;
                    materialColorSamplerAwaitMouseRelease = false;
                }
            }
        }
        if (runtime2DProfileThisFrame) {
            profileActualDrawMs += Runtime2DMsSince(drawRenderStart, Runtime2DClock::now());
        }

        // Enforce cursor lock state at the end of the frame based on latest flags.
        updateRuntimeMouseCapture();
        bool anyLock = cursorLocked || gameViewCursorLocked;
        int desiredMode = anyLock ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL;
        if (glfwGetInputMode(editorWindow, GLFW_CURSOR) != desiredMode) {
            glfwSetInputMode(editorWindow, GLFW_CURSOR, desiredMode);
            if (anyLock && glfwRawMouseMotionSupported()) {
                glfwSetInputMode(editorWindow, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
            } else if (!anyLock && glfwRawMouseMotionSupported()) {
                glfwSetInputMode(editorWindow, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
            }
        }
        updateRuntimeCursorImage();

        if (!usingVulkan()) {
            static int presentProbe = -1;
            if (presentProbe < 0) {
                const char* v = std::getenv("MODULARITY_PRESENT_PROBE");
                presentProbe = (v && *v && *v != '0') ? 1 : 0;
#ifndef __ANDROID__
                if (presentProbe) {
                    glfwSwapInterval(0);
                    Profiler::instance().setRecording(true);
                }
#endif
            }
            auto presentBuffers = [&]() {
                MODU_PROFILE_SCOPE("Swap Buffers", ProfilerSampleCategory::Render);
#ifdef __ANDROID__
                if (xrFrameSubmitted) return;
                Modularity::AndroidRuntime::PresentFrame();
#else
                if (presentProbe) {
                    static double drainAccum = 0.0, swapAccum = 0.0, probeStart = 0.0;
                    static int probeFrames = 0;
                    const double t0 = glfwGetTime();
                    if (probeStart <= 0.0) probeStart = t0;
                    glFinish();
                    const double t1 = glfwGetTime();
                    glfwSwapBuffers(editorWindow);
                    const double t2 = glfwGetTime();
                    drainAccum += (t1 - t0) * 1000.0;
                    swapAccum += (t2 - t1) * 1000.0;
                    ++probeFrames;
                    if (t2 - probeStart >= 5.0 && probeFrames > 0) {
                        double gpuSum = 0.0;
                        int gpuCount = 0;
                        const size_t historyCount = profiler.getHistoryCount();
                        for (size_t hi = 0; hi < historyCount; ++hi) {
                            const ProfilerFrameRecord* rec = profiler.getHistoryFrame(hi);
                            if (rec && rec->gpuMs > 0.0) {
                                gpuSum += rec->gpuMs;
                                ++gpuCount;
                            }
                        }
                        std::fprintf(stderr,
                                     "[PresentProbe] gpu-drain %.3f ms | swap %.3f ms | "
                                     "profiler-gpu %.3f ms (%d frames sampled) | %d frames\n",
                                     drainAccum / probeFrames, swapAccum / probeFrames,
                                     gpuCount > 0 ? gpuSum / gpuCount : 0.0, gpuCount, probeFrames);
                        drainAccum = swapAccum = 0.0;
                        probeFrames = 0;
                        probeStart = t2;
                    }
                } else {
                    glfwSwapBuffers(editorWindow);
                }
#endif
            };
            if (runtime2DProfileThisFrame) {
                const auto presentStart = Runtime2DClock::now();
                presentBuffers();
                gRuntime2DProfileFrame.presentWaitMs += Runtime2DMsSince(presentStart, Runtime2DClock::now());
            } else {
                presentBuffers();
            }
            Modularity::GLSyncProbe::OnFramePresented();
            if (renderResumeLogPending) {
                renderResumeLogPending = false;
                int resumeFbW = 0, resumeFbH = 0;
                glfwGetFramebufferSize(editorWindow, &resumeFbW, &resumeFbH);
                logWindowLifecycle("first-frame-ok", false, resumeFbW, resumeFbH);
            }
        }

        {
            MODU_PROFILE_SCOPE("Frame Pacing", ProfilerSampleCategory::Render);
            if (runtime2DProfileThisFrame) {
                const auto sleepStart = Runtime2DClock::now();
                paceFrame(frameStart);
                gRuntime2DProfileFrame.fpsCapSleepMs += Runtime2DMsSince(sleepStart, Runtime2DClock::now());
            } else {
                paceFrame(frameStart);
            }
        }

#if !MODULARITY_RUNTIME_ONLY && !defined(__ANDROID__)
        {
            // Low performance mode: an editor nobody is touching has nothing new to show, but it
            // would otherwise redraw both viewports at full rate and keep a weak laptop's CPU and
            // GPU pinned. Once input has been quiet for a moment, block on the event queue instead
            // of sleeping - any input wakes it at once, so interaction latency is unchanged.
            // MODULARITY_EDITOR_IDLE_THROTTLE=0 turns it off.
            constexpr double kIdleGraceSec = 1.0;
            constexpr double kIdleFocusedInterval = 1.0 / 15.0;
            constexpr double kIdleUnfocusedInterval = 1.0 / 8.0;
            static const bool idleThrottleAllowed = [] {
                const char* v = std::getenv("MODULARITY_EDITOR_IDLE_THROTTLE");
                return !(v && v[0] == '0' && v[1] == '\0');
            }();
            static double lastEditorActivityTime = 0.0;

            const double now = glfwGetTime();
            const bool editorBusy =
                playerMode || isPlaying || specMode || testMode || showLauncher ||
                projectLoadInProgress || sceneLoadInProgress || xrSystem.isRunning() ||
                cursorLocked || gameViewCursorLocked || EditorFrameHadUserInput();
            if (editorBusy) {
                lastEditorActivityTime = now;
            } else if (idleThrottleAllowed &&
                       now - lastEditorActivityTime > kIdleGraceSec &&
                       projectManager.preferences.effectiveTier() ==
                           Modularity::HardwareProfile::Tier::Low) {
                MODU_PROFILE_SCOPE("Idle Throttle", ProfilerSampleCategory::Render);
                const double interval = glfwGetWindowAttrib(editorWindow, GLFW_FOCUSED)
                                            ? kIdleFocusedInterval
                                            : kIdleUnfocusedInterval;
                const double remaining = frameStart + interval - now;
                if (remaining > 0.0) {
                    glfwWaitEventsTimeout(remaining);
                }
            }
        }
#endif

        if (runtime2DProfileThisFrame) {
            gRuntime2DProfileFrame.renderSubmissionMs += profileRenderSubmissionMs;
            gRuntime2DProfileFrame.actualDrawRenderMs += profileActualDrawMs;

            uint64_t rendererTextureBinds = 0;
            uint64_t rendererStateBinds = 0;
            ModuRuntime2DRenderCounters_Read(&rendererTextureBinds, &rendererStateBinds);

            uint64_t rendererDrawCalls = 0;
            if (rendererInitialized) {
                rendererDrawCalls = static_cast<uint64_t>(std::max(0, renderer.getLastViewportStats().drawCalls));
            }

            gRuntime2DProfileFrame.drawCallCount += rendererDrawCalls + imguiDrawCounters.drawCalls;
            gRuntime2DProfileFrame.textureBindCount += rendererTextureBinds + imguiDrawCounters.textureBinds;
            gRuntime2DProfileFrame.stateBindCount += rendererStateBinds + imguiDrawCounters.stateBinds;
            gRuntime2DProfileFrame.totalFrameMs += Runtime2DMsSince(frameStartClock, Runtime2DClock::now());

            accumulateRuntime2DFrame();
            emitRuntime2DProfileSummaryIfReady(glfwGetTime());
        }

        uint64_t profilerDrawCalls = 0;
        uint64_t profilerTextureBinds = 0;
        uint64_t profilerStateBinds = 0;
        if (rendererInitialized && (playerMode || showGameViewport)) {
            const SceneObject* profiledCameraObject = findPlayerCameraObject();
            const bool pure2DFrame =
                isProject2DPipeline() ||
                (profiledCameraObject && profiledCameraObject->camera.use2D);
            if (!pure2DFrame) {
                const Renderer::RenderStats& stats = playerMode
                    ? renderer.getLastViewportStats()
                    : renderer.getLastPreviewStats();
                profilerDrawCalls =
                    static_cast<uint64_t>(std::max(0, stats.drawCalls));
            }
        }
        if (runtime2DProfileThisFrame) {
            profilerTextureBinds = gRuntime2DProfileFrame.textureBindCount;
            profilerStateBinds = gRuntime2DProfileFrame.stateBindCount;
        }
        profiler.setCurrentFrameRenderCounters(profilerDrawCalls, profilerTextureBinds, profilerStateBinds);
        const Modu2DStats::Snapshot frame2DStats = Modu2DStats::Read();
        profiler.setCurrentFrame2DCounters(frame2DStats.spriteQuads,
                                           frame2DStats.spriteBatches,
                                           frame2DStats.postFxPasses,
                                           frame2DStats.viewportRedraws,
                                           frame2DStats.skippedRedraws,
                                           frame2DStats.cachedLayerReuses,
                                           frame2DStats.uiDirScans);
        profiler.setCurrentFrameRenderMemory(renderer.getTextureCacheUsageBytes(),
                                             renderer.getTextureCacheBudgetBytes());
        if (usingVulkan()) {
            profiler.setCurrentFrameGpuCapability(false, false);
        }
        profiler.endFrame();
        const double __frameMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - __frameWall).count();
        if (__frameMs > Modularity::HardwareProfile::TimingBudget(
                           projectLoadTraceActive ? 40.0 : 200.0)) {
            std::fprintf(stderr, "[ModuTimer] FRAME #%llu took %.1f ms (showLauncher=%d sceneLoadInProgress=%d)\n",
                         (unsigned long long)renderFrameSerial, __frameMs,
                         showLauncher ? 1 : 0, sceneLoadInProgress ? 1 : 0);
        }
        static const bool fpsLogEnabled = []() {
            const char* v = std::getenv("MODULARITY_FPS_LOG");
            return v && *v && *v != '0';
        }();
        if (fpsLogEnabled) {
            static double fpsWindowStart = glfwGetTime();
            static uint64_t fpsWindowFrames = 0;
            static double fpsWindowWorstMs = 0.0;
            ++fpsWindowFrames;
            fpsWindowWorstMs = std::max(fpsWindowWorstMs, __frameMs);
            const double fpsNowSec = glfwGetTime();
            const double fpsElapsed = fpsNowSec - fpsWindowStart;
            if (fpsElapsed >= 5.0) {
                std::fprintf(stderr,
                             "[FPSLog] avg %.0f fps (%.3f ms/frame, worst %.2f ms) over %llu frames\n",
                             fpsWindowFrames / fpsElapsed,
                             1000.0 * fpsElapsed / static_cast<double>(std::max<uint64_t>(1, fpsWindowFrames)),
                             fpsWindowWorstMs,
                             (unsigned long long)fpsWindowFrames);
                fpsWindowStart = fpsNowSec;
                fpsWindowFrames = 0;
                fpsWindowWorstMs = 0.0;
            }
        }
        firstFrame = false;
    }
}

Engine::~Engine() {
    // Also covers exception unwinding or callers that omit shutdown(). Member
    // futures otherwise join after the mutexes their workers use are destroyed.
    waitForBackgroundJobs();
    // Window is the first member and dies last. Keep the main GL context current
    // through destruction of the renderer and the other GPU-owning members.
    if (editorWindow && !usingVulkan()) glfwMakeContextCurrent(editorWindow);
    g_objLoader.clear(); // These global GPU owners otherwise survive Window.
    ModelLoader::getInstance().clear();
}

void Engine::waitForBackgroundJobs() {
    // Export workers capture this and use exportMutex/status fields. Drain
    // them while those fields and all backend resources are still alive.
    // Do not hold exportMutex while waiting: the worker needs it to finish.
    exportCancelRequested.store(true);
    if (exportJob.future.valid()) exportJob.future.wait();
    for (auto& worker : compileWorkers) {
        if (worker.valid()) worker.wait();
    }
    compileWorkers.clear();
}

void Engine::shutdown() {
    waitForBackgroundJobs();
    if (editorWindow && !usingVulkan()) glfwMakeContextCurrent(editorWindow);
    Modularity::DiscordPresence::Shutdown();
    shutdownXRSystem();
#if !MODULARITY_RUNTIME_ONLY
    uiFontContexts.erase(vrEditor.context());
    vrEditor.shutdown();
#endif
    ImGuiContext* mainContext = ImGui::GetCurrentContext();
    if (mainContext && !playerMode && projectManager.currentProject.isLoaded && !showLauncher) {
        saveEditorUserSettings();
        saveWorkspaceLayout(currentWorkspace);
    }

    if (
#ifndef MODULARITY_PLAYER
        projectManager.currentProject.isLoaded && projectManager.currentProject.hasUnsavedChanges
#else
        false
#endif
    ) {
        saveCurrentScene(false);
    }
    Modularity::FlushDurableWrites();

    physics->onPlayStop();
    audio.onPlayStop();
    audio.shutdown();
    physics->shutdown();

    for (auto& entry : uiCanvas3DContexts) {
        if (!entry.second.context) continue;
        ImGui::SetCurrentContext(entry.second.context);
        if (entry.second.backendReady && !usingVulkan()) {
            ImGui_ImplOpenGL3_Shutdown();
            entry.second.backendReady = false;
        }
        uiFontContexts.erase(entry.second.context);
        ImGui::DestroyContext(entry.second.context);
    }
    uiCanvas3DContexts.clear();
    uiKeyboardCanvasId = -1;

    if (mainContext) {
        ImGui::SetCurrentContext(mainContext);
#if MODULARITY_HAS_VULKAN
        if (usingVulkan()) {
            if (vulkanRenderer) {
                vulkanRenderer->shutdownImGuiBackend();
            }
        } else
#endif
        {
            Modularity::UiGlassBlur::Shutdown();
            ImGui_ImplOpenGL3_Shutdown();
        }
        ImGui_ImplGlfw_Shutdown();
        uiFontContexts.erase(mainContext);
        ImGui::DestroyContext(mainContext);
    }

    if (vulkanRenderer) {
        vulkanRenderer->shutdown();
        vulkanRenderer.reset();
    }
    vulkanRendererInitialized = false;
    videoAssetPreviewPlayer.reset();
    videoAssetPreviewPath.clear();
    clearVideoPlayers();
    // GLFW termination belongs to Window, after all Engine GPU owners die.
}
#pragma endregion

#pragma region Asset Import
void Engine::importOBJToScene(const std::string& filepath, const std::string& objectName) {
    recordState("importOBJ");
    std::string errorMsg;
    int meshId = g_objLoader.loadOBJ(filepath, errorMsg);

    if (meshId < 0) {
        addConsoleMessage("Failed to load OBJ: " + errorMsg, ConsoleMessageType::Error);
        return;
    }

    int id = nextObjectId++;
    std::string name = objectName.empty() ? fs::path(filepath).stem().string() : objectName;

    SceneObject obj(name, ObjectType::Empty, id);
    obj.hasRenderer = true;
    obj.renderType = RenderType::OBJMesh;
    obj.type = ObjectType::OBJMesh;
    obj.meshPath = filepath;
    obj.meshId = meshId;

    sceneObjects.push_back(obj);
    setPrimarySelection(id);

    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }

    const auto* meshInfo = g_objLoader.getMeshInfo(meshId);
    if (meshInfo) {
        addConsoleMessage("Imported OBJ: " + name + " (" +
                        std::to_string(meshInfo->vertexCount) + " vertices, " +
                        std::to_string(meshInfo->faceCount) + " faces)",
                        ConsoleMessageType::Success);
    } else {
        addConsoleMessage("Imported OBJ: " + name, ConsoleMessageType::Success);
    }
}

void Engine::importModelToScene(const std::string& filepath, const std::string& objectName) {
    ModelTextureResolver textureResolver(
        projectManager.currentProject.isLoaded
            ? (projectManager.currentProject.assetsPath.empty()
                   ? projectManager.currentProject.projectPath
                   : projectManager.currentProject.assetsPath)
            : fs::path(),
        projectManager.currentProject.isLoaded ? projectManager.currentProject.projectPath
                                               : fs::path());
    if (IsMMeshPath(filepath)) {
        recordState("importMMesh");
        int id = nextObjectId++;
        std::string name = objectName.empty() ? fs::path(filepath).stem().string() : objectName;

        SceneObject obj(name, ObjectType::Empty, id);
        obj.hasRenderer = true;
        obj.renderType = RenderType::Model;
        obj.type = ObjectType::Model;
        obj.meshPath = filepath;
        obj.meshId = -1;
        sceneObjects.push_back(obj);
        markRuntimeScriptBindingsDirty();
        setPrimarySelection(id);
        if (projectManager.currentProject.isLoaded) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        addConsoleMessage("Imported MMesh: " + name, ConsoleMessageType::Success);
        return;
    }

    recordState("importModel");
    auto& modelLoader = getModelLoader();
    ModelSceneData sceneData;
    std::string error;
    if (!modelLoader.loadModelScene(filepath, sceneData, error)) {
        ModelLoadResult fallback = modelLoader.loadModel(filepath);
        if (!fallback.success) {
            addConsoleMessage("Failed to load model: " + error, ConsoleMessageType::Error);
            return;
        }
        int id = nextObjectId++;
        std::string name = objectName.empty() ? fs::path(filepath).stem().string() : objectName;
        SceneObject obj(name, ObjectType::Empty, id);
        obj.hasRenderer = true;
        obj.renderType = RenderType::Model;
        obj.type = ObjectType::Model;
        obj.meshPath = filepath;
        obj.meshId = fallback.meshIndex;
        sceneObjects.push_back(obj);
        setPrimarySelection(id);
        if (projectManager.currentProject.isLoaded) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        addConsoleMessage(
            "Imported model: " + name + " (" +
            std::to_string(fallback.vertexCount) + " verts, " +
            std::to_string(fallback.faceCount) + " faces, " +
            std::to_string(fallback.meshCount) + " meshes)",
            ConsoleMessageType::Success
        );
        return;
    }

    std::string baseName = objectName.empty() ? fs::path(filepath).stem().string() : objectName;
    std::vector<std::string> materialPaths(sceneData.materials.size());

    if (projectManager.currentProject.isLoaded && !sceneData.materials.empty()) {
        fs::path materialsDir = projectManager.currentProject.assetsPath / "Materials" / "Imported" / baseName;
        std::error_code ec;
        std::set<std::string> importedTexturePaths;
        fs::create_directories(materialsDir, ec);
        if (ec) {
            addConsoleMessage("Failed to create materials folder: " + materialsDir.string(), ConsoleMessageType::Warning);
        } else {
            for (size_t i = 0; i < sceneData.materials.size(); ++i) {
                const auto& mat = sceneData.materials[i];
                std::string matName = sanitizeMaterialName(mat.name);
                fs::path matPath = materialsDir / (matName + ".mat");
                MaterialFileData data;
                data.props = mat.props;
                data.albedo = textureResolver.resolve(mat.albedoPath, filepath,
                                                      ModelTextureResolver::Role::Albedo, mat.name);
                data.overlay.clear();
                data.normal = textureResolver.resolve(mat.normalPath, filepath,
                                                      ModelTextureResolver::Role::Normal, mat.name);
                data.roughnessMap = textureResolver.resolve(
                    "", filepath, ModelTextureResolver::Role::Roughness, mat.name);
                data.metallicMap = textureResolver.resolve(
                    "", filepath, ModelTextureResolver::Role::Metallic, mat.name);
                data.opacityMap = textureResolver.resolve(
                    "", filepath, ModelTextureResolver::Role::Opacity, mat.name);
                data.emissive = textureResolver.resolve(
                    "", filepath, ModelTextureResolver::Role::Emissive, mat.name);
                data.occlusion = textureResolver.resolve(
                    "", filepath, ModelTextureResolver::Role::Occlusion, mat.name);
                if (!data.metallicMap.empty() || !data.roughnessMap.empty()) {
                    data.props.pbrEnabled = true;
                }
                if (!data.emissive.empty() && data.props.emissionStrength <= 0.0f) {
                    data.props.emissionStrength = 1.5f;
                }
                data.useOverlay = false;
                data.vertexShader.clear();
                data.fragmentShader.clear();
                if (writeMaterialFile(data, matPath.string())) {
                    materialPaths[i] = matPath.string();
                }
                for (const std::string* slot : {&data.albedo, &data.normal,
                                                &data.roughnessMap, &data.metallicMap,
                                                &data.opacityMap, &data.emissive,
                                                &data.occlusion}) {
                    if (!slot->empty()) importedTexturePaths.insert(*slot);
                }
            }
        }

#if !MODULARITY_RUNTIME_ONLY
        int needTranscode = 0;
        for (const std::string& texPath : importedTexturePaths) {
            const fs::path p(texPath);
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (ext != ".exr") continue;
            const fs::path abs = p.is_absolute()
                                     ? p
                                     : (projectManager.currentProject.projectPath / p);
            if (Modularity::ResolveLoadableTexturePath(abs.string()) != abs.string()) continue;
            if (renderer.getTexture(abs.string()) != nullptr) continue;
            ++needTranscode;
        }
        if (needTranscode > 0) {
            addConsoleMessage(
                "Import pulled in " + std::to_string(needTranscode) +
                    " EXR texture(s) the bundled reader cannot decompress (Poly Haven ships "
                    "DWAA). Converting them through Blender now.",
                ConsoleMessageType::Info);
            transcodeProjectExrTextures();
        }
#endif
    }

    constexpr size_t kStaticBatchMeshThreshold = 16;
    size_t validMeshCount = 0;
    for (int meshId : sceneData.meshIndices) {
        if (meshId >= 0) {
            ++validMeshCount;
        }
    }

    bool hasSkinnedMeshes = false;
    for (int meshId : sceneData.meshIndices) {
        if (meshId < 0) continue;
        const auto* info = modelLoader.getMeshInfo(meshId);
        if (info && info->isSkinned) {
            hasSkinnedMeshes = true;
            break;
        }
    }

    bool hasAnimations = !sceneData.animations.empty();
    bool singleMaterial = sceneData.materials.size() <= 1;
    if (!singleMaterial && !sceneData.meshMaterialIndices.empty()) {
        int firstMat = -2;
        bool mixed = false;
        for (int matIndex : sceneData.meshMaterialIndices) {
            if (matIndex < 0) continue;
            if (firstMat == -2) {
                firstMat = matIndex;
            } else if (matIndex != firstMat) {
                mixed = true;
                break;
            }
        }
        singleMaterial = !mixed;
    }

    if (validMeshCount >= kStaticBatchMeshThreshold &&
        !hasSkinnedMeshes && !hasAnimations && singleMaterial) {
        RawMeshAsset raw;
        glm::vec3 rootPos(0.0f);
        glm::vec3 rootRot(0.0f);
        glm::vec3 rootScale(1.0f);
        if (modelLoader.buildRawMeshFromScene(filepath, raw, error, &rootPos, &rootRot, &rootScale)) {
            std::string batchName = baseName + "_StaticBatch";
            int batchMeshId = modelLoader.addRawMesh(raw, filepath, batchName, error);
            if (batchMeshId >= 0) {
                int id = nextObjectId++;
                SceneObject obj(baseName, ObjectType::Empty, id);
                obj.hasRenderer = true;
                obj.renderType = RenderType::Model;
                obj.type = ObjectType::Model;
                obj.meshPath = filepath;
                obj.meshId = batchMeshId;
                obj.meshSourceIndex = -1;
                obj.localPosition = rootPos;
                obj.localRotation = rootRot;
                obj.localScale = rootScale;
                obj.localInitialized = true;
                obj.position = obj.localPosition;
                obj.rotation = obj.localRotation;
                obj.scale = obj.localScale;

                if (!materialPaths.empty() && !materialPaths[0].empty()) {
                    obj.materialPath = materialPaths[0];
                    loadMaterialFromFile(obj);
                } else if (!sceneData.materials.empty()) {
                    const auto& mat = sceneData.materials.front();
                    obj.material = mat.props;
                    obj.albedoTexturePath = textureResolver.resolve(
                        mat.albedoPath, filepath, ModelTextureResolver::Role::Albedo, mat.name);
                    obj.normalMapPath = textureResolver.resolve(
                        mat.normalPath, filepath, ModelTextureResolver::Role::Normal, mat.name);
                }

                sceneObjects.push_back(obj);
                setPrimarySelection(id);
                if (projectManager.currentProject.isLoaded) {
                    projectManager.currentProject.hasUnsavedChanges = true;
                }
                addConsoleMessage(
                    "Imported model (static batch): " + baseName + " (" +
                    std::to_string(validMeshCount) + " meshes)",
                    ConsoleMessageType::Success
                );
                return;
            }
        }
    }

    std::vector<int> nodeObjectIds(sceneData.nodes.size(), -1);
    int rootSelectionId = -1;

    for (size_t i = 0; i < sceneData.nodes.size(); ++i) {
        const auto& node = sceneData.nodes[i];
        std::string nodeName = node.name.empty()
            ? (baseName + "_Node" + std::to_string(i))
            : node.name;
        SceneObject obj(nodeName, ObjectType::Empty, nextObjectId++);
        obj.localPosition = node.localPosition;
        obj.localRotation = node.localRotation;
        obj.localScale = node.localScale;
        obj.localInitialized = true;
        obj.position = obj.localPosition;
        obj.rotation = obj.localRotation;
        obj.scale = obj.localScale;
        sceneObjects.push_back(obj);
        nodeObjectIds[i] = obj.id;
        if (rootSelectionId == -1) rootSelectionId = obj.id;
    }

    for (size_t i = 0; i < sceneData.nodes.size(); ++i) {
        int parentIndex = sceneData.nodes[i].parentIndex;
        if (parentIndex < 0 || parentIndex >= (int)nodeObjectIds.size()) continue;
        int parentId = nodeObjectIds[parentIndex];
        int childId = nodeObjectIds[i];
        if (parentId < 0 || childId < 0) continue;
        SceneObject* parentObj = findObjectById(parentId);
        SceneObject* childObj = findObjectById(childId);
        if (!parentObj || !childObj) continue;
        childObj->parentId = parentId;
        parentObj->childIds.push_back(childId);
    }

    std::vector<std::string> sceneNodePaths(sceneData.nodes.size());
    std::unordered_map<std::string, int> nodeIdByPath;
    nodeIdByPath.reserve(sceneData.nodes.size());
    for (size_t i = 0; i < sceneData.nodes.size(); ++i) {
        std::vector<std::string> segments;
        int cursor = static_cast<int>(i);
        while (cursor >= 0 && cursor < static_cast<int>(sceneData.nodes.size())) {
            if (cursor != 0) {
                segments.push_back(sceneData.nodes[static_cast<size_t>(cursor)].name);
            }
            cursor = sceneData.nodes[static_cast<size_t>(cursor)].parentIndex;
        }
        std::string path;
        for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
            if (!path.empty()) path += '/';
            path += *it;
        }
        sceneNodePaths[i] = path;
        if (nodeObjectIds[i] >= 0) {
            nodeIdByPath[path] = nodeObjectIds[i];
        }
    }

    for (size_t nodeIndex = 0; nodeIndex < sceneData.nodes.size(); ++nodeIndex) {
        const auto& node = sceneData.nodes[nodeIndex];
        int parentId = nodeObjectIds[nodeIndex];
        if (parentId < 0) continue;
        for (int meshSourceIndex : node.meshIndices) {
            if (meshSourceIndex < 0 || meshSourceIndex >= (int)sceneData.meshIndices.size()) continue;
            int meshId = sceneData.meshIndices[meshSourceIndex];
            if (meshId < 0) continue;

            const auto* meshInfo = modelLoader.getMeshInfo(meshId);
            std::string meshName = meshInfo && !meshInfo->name.empty()
                ? meshInfo->name
                : (baseName + "_Mesh");

            SceneObject meshObj(meshName, ObjectType::Empty, nextObjectId++);
            meshObj.hasRenderer = true;
            meshObj.renderType = RenderType::Model;
            meshObj.type = ObjectType::Model;
            meshObj.meshPath = filepath;
            meshObj.meshId = meshId;
            meshObj.meshSourceIndex = meshSourceIndex;
            meshObj.localPosition = glm::vec3(0.0f);
            meshObj.localRotation = glm::vec3(0.0f);
            meshObj.localScale = glm::vec3(1.0f);
            meshObj.localInitialized = true;
            meshObj.position = meshObj.localPosition;
            meshObj.rotation = meshObj.localRotation;
            meshObj.scale = meshObj.localScale;
            meshObj.parentId = parentId;

            if (meshInfo && meshInfo->isSkinned) {
                meshObj.hasSkeletalAnimation = true;
                meshObj.skeletal = SkeletalAnimationComponent{};
                meshObj.skeletal.useAnimation = false;
                meshObj.skeletal.skeletonRootId = rootSelectionId;
                meshObj.skeletal.boneNames = meshInfo->boneNames;
                meshObj.skeletal.inverseBindMatrices = meshInfo->inverseBindMatrices;
                meshObj.skeletal.finalMatrices.assign(meshInfo->boneNames.size(), glm::mat4(1.0f));
                meshObj.skeletal.boneNodeIds.assign(meshInfo->boneNames.size(), -1);
                meshObj.skeletal.armatureNodeIds.reserve(nodeObjectIds.size());
                for (int nodeId : nodeObjectIds) {
                    if (nodeId >= 0) {
                        meshObj.skeletal.armatureNodeIds.push_back(nodeId);
                    }
                }
                for (size_t b = 0; b < meshInfo->boneNames.size(); ++b) {
                    if (b < meshInfo->boneNodePaths.size() && !meshInfo->boneNodePaths[b].empty()) {
                        auto itPath = nodeIdByPath.find(meshInfo->boneNodePaths[b]);
                        if (itPath != nodeIdByPath.end()) {
                            meshObj.skeletal.boneNodeIds[b] = itPath->second;
                            continue;
                        }
                    }
                    for (size_t n = 0; n < sceneData.nodes.size(); ++n) {
                        if (sceneData.nodes[n].name == meshInfo->boneNames[b]) {
                            int nodeId = nodeObjectIds[n];
                            if (nodeId >= 0) {
                                meshObj.skeletal.boneNodeIds[b] = nodeId;
                            }
                            break;
                        }
                    }
                }
            }

            int matIndex = (meshSourceIndex >= 0 && meshSourceIndex < (int)sceneData.meshMaterialIndices.size())
                ? sceneData.meshMaterialIndices[meshSourceIndex]
                : -1;
            if (matIndex >= 0 && matIndex < (int)materialPaths.size() && !materialPaths[matIndex].empty()) {
                meshObj.materialPath = materialPaths[matIndex];
                loadMaterialFromFile(meshObj);
            } else if (matIndex >= 0 && matIndex < (int)sceneData.materials.size()) {
                const auto& mat = sceneData.materials[matIndex];
                meshObj.material = mat.props;
                meshObj.albedoTexturePath = textureResolver.resolve(
                    mat.albedoPath, filepath, ModelTextureResolver::Role::Albedo, mat.name);
                meshObj.normalMapPath = textureResolver.resolve(
                    mat.normalPath, filepath, ModelTextureResolver::Role::Normal, mat.name);
            }

            sceneObjects.push_back(meshObj);
            if (SceneObject* parentObj = findObjectById(parentId)) {
                parentObj->childIds.push_back(meshObj.id);
            }
        }
    }

    int exportedClipCount = 0;
    if (projectManager.currentProject.isLoaded && rootSelectionId != -1 && !sceneData.animations.empty()) {
        std::unordered_map<std::string, std::string> pathByNodeName;
        pathByNodeName.reserve(sceneData.nodes.size());
        std::unordered_map<std::string, size_t> nodeIndexByName;
        nodeIndexByName.reserve(sceneData.nodes.size());
        for (size_t i = 0; i < sceneData.nodes.size(); ++i) {
            pathByNodeName[sceneData.nodes[i].name] = sceneNodePaths[i];
            nodeIndexByName[sceneData.nodes[i].name] = i;
        }

        fs::path animationDir = projectManager.currentProject.assetsPath / "Animations" / "Imported" / baseName;
        std::error_code ec;
        fs::create_directories(animationDir, ec);
        if (!ec) {
            SceneObject* rootObj = findObjectById(rootSelectionId);
            if (rootObj) {
                rootObj->hasAnimation = true;
                rootObj->animation.enabled = true;
                rootObj->animation.playOnAwake = false;
                rootObj->animation.runtimePlaying = false;
                rootObj->animation.runtimeInitialized = false;
                rootObj->animation.clips.clear();

                for (size_t clipIndex = 0; clipIndex < sceneData.animations.size(); ++clipIndex) {
                    const auto& clip = sceneData.animations[clipIndex];
                    std::string clipName = clip.name.empty()
                        ? ("Clip_" + std::to_string(clipIndex))
                        : clip.name;
                    fs::path outPath = animationDir / (sanitizeMaterialName(clipName) + ".moduanimate");
                    std::ofstream out(outPath, std::ios::trunc);
                    if (!out.is_open()) continue;

                    const float sampleRate = static_cast<float>(std::clamp(clip.ticksPerSecond, 1.0, 240.0));
                    const float durationSeconds = clip.ticksPerSecond > 0.0
                        ? static_cast<float>(clip.duration / clip.ticksPerSecond)
                        : static_cast<float>(clip.duration / 25.0);

                    out << "moduanimateVersion 2\n";
                    out << "name " << std::quoted(clipName) << "\n";
                    out << "rootObjectId " << rootSelectionId << "\n";
                    out << "duration " << std::max(0.01f, durationSeconds) << "\n";
                    out << "sampleRate " << sampleRate << "\n";

                    std::vector<const ModelSceneData::AnimChannel*> exportedChannels;
                    for (const auto& channel : clip.channels) {
                        if (pathByNodeName.find(channel.nodeName) != pathByNodeName.end()) {
                            exportedChannels.push_back(&channel);
                        }
                    }

                    out << "bindingCount " << exportedChannels.size() << "\n";
                    uint64_t uid = 1;
                    auto writeVecTrack = [&](const char* propertyId, const std::vector<ModelSceneData::AnimVecKey>& keys, int component) {
                        out << "track " << std::quoted(propertyId) << " 1 0\n";
                        out << "keyCount " << keys.size() << "\n";
                        for (const auto& key : keys) {
                            const float t = clip.ticksPerSecond > 0.0
                                ? key.time / static_cast<float>(clip.ticksPerSecond)
                                : key.time / 25.0f;
                            out << "key " << uid++ << " " << t << " " << key.value[component]
                                << " 0 0 2 1\n";
                        }
                    };
                    auto unwrapEulerNear = [](float angle, float reference) {
                        float result = angle;
                        while (result - reference > 180.0f) result -= 360.0f;
                        while (reference - result > 180.0f) result += 360.0f;
                        return result;
                    };
                    auto buildContinuousEulerKeys = [&](const ModelSceneData::AnimChannel& channel) {
                        std::vector<glm::vec3> result;
                        result.reserve(channel.rotations.size());
                        glm::vec3 previous(0.0f);
                        auto itNode = nodeIndexByName.find(channel.nodeName);
                        if (itNode != nodeIndexByName.end()) {
                            previous = sceneData.nodes[itNode->second].localRotation;
                        }
                        bool havePrevious = true;
                        for (const auto& key : channel.rotations) {
                            glm::vec3 euler = glm::degrees(ExtractEulerXYZ(glm::mat3_cast(glm::normalize(key.value))));
                            if (havePrevious) {
                                euler.x = unwrapEulerNear(euler.x, previous.x);
                                euler.y = unwrapEulerNear(euler.y, previous.y);
                                euler.z = unwrapEulerNear(euler.z, previous.z);
                            }
                            result.push_back(euler);
                            previous = euler;
                            havePrevious = true;
                        }
                        return result;
                    };

                    for (const ModelSceneData::AnimChannel* channel : exportedChannels) {
                        out << "binding " << std::quoted(pathByNodeName[channel->nodeName]) << " " << std::quoted("SceneObject") << "\n";
                        const size_t trackCount =
                            (channel->positions.empty() ? 0u : 3u) +
                            (channel->rotations.empty() ? 0u : 3u) +
                            (channel->scales.empty() ? 0u : 3u);
                        out << "trackCount " << trackCount << "\n";
                        if (!channel->positions.empty()) {
                            writeVecTrack("localPosition.x", channel->positions, 0);
                            writeVecTrack("localPosition.y", channel->positions, 1);
                            writeVecTrack("localPosition.z", channel->positions, 2);
                        }
                        if (!channel->rotations.empty()) {
                            const std::vector<glm::vec3> eulerKeys = buildContinuousEulerKeys(*channel);
                            auto writeContinuousRotTrack = [&](const char* propertyId, int component) {
                                out << "track " << std::quoted(propertyId) << " 1 0\n";
                                out << "keyCount " << channel->rotations.size() << "\n";
                                for (size_t k = 0; k < channel->rotations.size(); ++k) {
                                    const auto& key = channel->rotations[k];
                                    const float t = clip.ticksPerSecond > 0.0
                                        ? key.time / static_cast<float>(clip.ticksPerSecond)
                                        : key.time / 25.0f;
                                    out << "key " << uid++ << " " << t << " " << eulerKeys[k][component]
                                        << " 0 0 2 1\n";
                                }
                            };
                            writeContinuousRotTrack("localRotation.x", 0);
                            writeContinuousRotTrack("localRotation.y", 1);
                            writeContinuousRotTrack("localRotation.z", 2);
                        }
                        if (!channel->scales.empty()) {
                            writeVecTrack("localScale.x", channel->scales, 0);
                            writeVecTrack("localScale.y", channel->scales, 1);
                            writeVecTrack("localScale.z", channel->scales, 2);
                        }
                    }

                    if (out.good()) {
                        fs::path storedPath = outPath;
                        if (!projectManager.currentProject.projectPath.empty()) {
                            std::error_code relEc;
                            fs::path rel = fs::relative(outPath, projectManager.currentProject.projectPath, relEc);
                            if (!relEc && !rel.empty()) {
                                storedPath = rel;
                            }
                        }
                        AnimationClipSlot slot;
                        slot.name = clipName;
                        slot.assetPath = storedPath.generic_string();
                        rootObj->animation.clips.push_back(std::move(slot));
                        ++exportedClipCount;
                    }
                }

                if (!rootObj->animation.clips.empty()) {
                    rootObj->animation.activeClipIndex = 0;
                    rootObj->animation.clipAssetPath = rootObj->animation.clips.front().assetPath;
                } else {
                    rootObj->hasAnimation = false;
                }
            }
        } else {
            addConsoleMessage("Failed to create imported animation folder: " + animationDir.string(), ConsoleMessageType::Warning);
        }
    }

    updateHierarchyWorldTransforms();
    if (rootSelectionId != -1) {
        setPrimarySelection(rootSelectionId);
    }

    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }

    addConsoleMessage(
        "Imported model: " + baseName + " (" +
        std::to_string(sceneData.meshIndices.size()) + " meshes, " +
        std::to_string(sceneData.nodes.size()) + " nodes" +
        (exportedClipCount > 0 ? ", " + std::to_string(exportedClipCount) + " animation clips" : "") + ")",
        ConsoleMessageType::Success
    );

    if (textureResolver.discoveredByConvention() > 0) {
        addConsoleMessage(
            "Found " + std::to_string(textureResolver.discoveredByConvention()) +
            " texture(s) by name convention (the model itself referenced none).",
            ConsoleMessageType::Info
        );
    }
    if (textureResolver.resolvedByName() > 0) {
        addConsoleMessage(
            "Matched " + std::to_string(textureResolver.resolvedByName()) +
            " texture(s) by name from the project (the model referenced paths that do not exist here).",
            ConsoleMessageType::Info
        );
    }
    if (textureResolver.unresolved() > 0) {
        addConsoleMessage(
            std::to_string(textureResolver.unresolved()) +
            " texture(s) could not be found by path or by name. Import them into the project "
            "and reassign, or check the model's texture names match the files.",
            ConsoleMessageType::Warning
        );
    }
}

void Engine::convertModelToRawMesh(const std::string& filepath) {
    auto& modelLoader = getModelLoader();
    fs::path inPath(filepath);
    fs::path outPath = inPath;
    outPath.replace_extension(".rmesh");

    std::string error;
    if (modelLoader.exportRawMesh(filepath, outPath.string(), error)) {
        addConsoleMessage("Converted to raw mesh: " + outPath.string(), ConsoleMessageType::Success);
        fileBrowser.needsRefresh = true;
    } else {
        addConsoleMessage("Raw mesh export failed: " + error, ConsoleMessageType::Error);
    }
}

void Engine::createRMeshPrimitive(const std::string& primitiveName) {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("Load a project before creating RMesh primitives", ConsoleMessageType::Warning);
        return;
    }

    fs::path root = projectManager.currentProject.assetsPath / "Models" / "RMeshes" / "Primitives";
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        addConsoleMessage("Failed to create RMesh folder: " + root.string(), ConsoleMessageType::Error);
        return;
    }

    RawMeshAsset asset;
    if (primitiveName == "Cube") {
        asset = buildCubeRMesh();
    } else if (primitiveName == "Sphere") {
        asset = buildSphereRMesh();
    } else if (primitiveName == "Plane") {
        asset = buildPlaneRMesh();
    } else {
        addConsoleMessage("Unknown RMesh primitive: " + primitiveName, ConsoleMessageType::Warning);
        return;
    }

    fs::path filePath = root / (primitiveName + ".rmesh");
    if (fs::exists(filePath)) {
        int suffix = 1;
        fs::path candidate;
        do {
            candidate = root / (primitiveName + "_" + std::to_string(suffix) + ".rmesh");
            ++suffix;
        } while (fs::exists(candidate));
        filePath = candidate;
    }

    std::string error;
    if (!getModelLoader().saveRawMesh(asset, filePath.string(), error)) {
        addConsoleMessage("Failed to save RMesh primitive: " + error, ConsoleMessageType::Error);
        return;
    }
    fileBrowser.needsRefresh = true;

    importModelToScene(filePath.string(), primitiveName);
}

void Engine::createMMeshPrimitive(const std::string& primitiveName) {
    using namespace Modularity::Render25D;

    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("Load a project before creating MMesh primitives", ConsoleMessageType::Warning);
        return;
    }

    fs::path root = projectManager.currentProject.assetsPath / "Models" / "MMeshes" / "Primitives";
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        addConsoleMessage("Failed to create MMesh folder: " + root.string(), ConsoleMessageType::Error);
        return;
    }

    RawMeshAsset raw;
    if (primitiveName == "Cube") {
        raw = buildCubeRMesh();
    } else if (primitiveName == "Sphere") {
        raw = buildSphereRMesh();
    } else if (primitiveName == "Plane") {
        raw = buildPlaneRMesh();
    } else {
        addConsoleMessage("Unknown MMesh primitive: " + primitiveName, ConsoleMessageType::Warning);
        return;
    }

    MMeshAsset asset = BuildMMeshFromRawMesh(raw);
    MMeshLoader loader;

    fs::path filePath = root / (primitiveName + ".mmesh");
    if (fs::exists(filePath)) {
        int suffix = 1;
        fs::path candidate;
        do {
            candidate = root / (primitiveName + "_" + std::to_string(suffix) + ".mmesh");
            ++suffix;
        } while (fs::exists(candidate));
        filePath = candidate;
    }

    std::string error;
    if (!loader.saveAsset(asset, filePath.string(), error)) {
        addConsoleMessage("Failed to save MMesh primitive: " + error, ConsoleMessageType::Error);
        return;
    }

    fileBrowser.needsRefresh = true;
    importModelToScene(filePath.string(), primitiveName);
}

void Engine::createPipelineDefaultSceneObjects() {
    if (isProject25DPipeline()) {
        addObject(ObjectType::Plane, "TM Floor");
        if (!sceneObjects.empty()) {
            SceneObject& floor = sceneObjects.back();
            floor.position = glm::vec3(0.0f, 0.0f, 0.0f);
            floor.rotation = glm::vec3(270.0f, 0.0f, 0.0f);
            floor.scale = glm::vec3(32.0f, 32.0f, 1.0f);
            floor.localPosition = floor.position;
            floor.localRotation = NormalizeEulerDegrees(floor.rotation);
            floor.localScale = floor.scale;
        }
        return;
    }

    addObject(ObjectType::Cube, "Cube");
}
#pragma endregion

#pragma region Mesh Editing
bool Engine::ensureMeshEditTarget(SceneObject* obj) {
    if (!obj) return false;
    if (!IsMeshEditablePath(obj->meshPath)) return false;

    if (meshEditLoaded && meshEditPath == obj->meshPath) {
        if (meshEditAsset.materialSlots.empty()) {
            meshEditAsset.materialSlots.push_back("Default");
        }
        if (meshEditAsset.faceMaterialIndices.size() != meshEditAsset.faces.size()) {
            meshEditAsset.faceMaterialIndices.resize(meshEditAsset.faces.size(), 0u);
        }
        if (meshEditActiveMaterialSlot < 0 || meshEditActiveMaterialSlot >= static_cast<int>(meshEditAsset.materialSlots.size())) {
            meshEditActiveMaterialSlot = 0;
        }
        return true;
    }

    std::string err;
    if (!getModelLoader().loadRawMesh(obj->meshPath, meshEditAsset, err)) {
        addConsoleMessage("Mesh edit load failed: " + err, ConsoleMessageType::Error);
        meshEditLoaded = false;
        return false;
    }
    meshEditLoaded = true;
    meshEditPath = obj->meshPath;
    meshEditDirty = false;
    if (meshEditAsset.materialSlots.empty()) {
        meshEditAsset.materialSlots.push_back("Default");
    }
    if (meshEditAsset.faceMaterialIndices.size() != meshEditAsset.faces.size()) {
        meshEditAsset.faceMaterialIndices.resize(meshEditAsset.faces.size(), 0u);
    }
    meshEditActiveMaterialSlot = std::clamp(meshEditActiveMaterialSlot, 0, static_cast<int>(meshEditAsset.materialSlots.size()) - 1);
    meshEditSelectedVertices.clear();
    meshEditSelectedEdges.clear();
    meshEditSelectedFaces.clear();
    return true;
}

bool Engine::syncMeshEditToGPU(SceneObject* obj) {
    if (!obj || !meshEditLoaded) return false;
    if (obj->meshId < 0 && !obj->meshPath.empty()) {
        ModelLoadResult loaded = getModelLoader().loadModel(obj->meshPath);
        if (loaded.success) {
            obj->meshId = loaded.meshIndex;
        } else {
            addConsoleMessage("Mesh GPU sync failed: " + loaded.errorMessage, ConsoleMessageType::Error);
            return false;
        }
    }
    std::string err;
    if (!getModelLoader().updateRawMesh(obj->meshId, meshEditAsset, err)) {
        addConsoleMessage("Mesh GPU sync failed: " + err, ConsoleMessageType::Error);
        return false;
    }

    if (obj->hasCollider && obj->collider.enabled &&
        (obj->collider.type == ColliderType::Mesh ||
         obj->collider.type == ColliderType::ConvexMesh) &&
        physics && physics->isReady() && !physics->refreshObject(*obj)) {
        addConsoleMessage("Mesh collider refresh failed for " + obj->name,
                          ConsoleMessageType::Warning);
    }

    if (IsMMeshPath(obj->meshPath)) {
        using namespace Modularity::Render25D;
        MMeshAsset live = BuildMMeshFromRawMesh(meshEditAsset);
        std::string cacheError;
        const MMeshRenderData* existing = tmRenderer.getMeshCache().getOrLoad(obj->meshPath, cacheError);
        if (existing != nullptr) {
            for (MMeshMaterialRef& material : live.materials) {
                for (const MMeshRenderSubmesh& submesh : existing->submeshes) {
                    if (submesh.materialName == material.name) {
                        material.albedoTexturePath = submesh.albedoTexturePath;
                        material.presentation = submesh.presentation;
                        break;
                    }
                }
            }
        }
        cacheError.clear();
        if (tmRenderer.getMeshCache().store(obj->meshPath, live, cacheError) == nullptr) {
            addConsoleMessage("MMesh cache refresh failed: " + cacheError, ConsoleMessageType::Error);
        }
    }

    projectManager.currentProject.hasUnsavedChanges = true;
    return true;
}

bool Engine::saveMeshEditAsset(std::string& error) {
    if (!meshEditLoaded) {
        error = "No mesh loaded for editing";
        return false;
    }
    if (meshEditPath.empty()) {
        error = "Mesh edit path is empty";
        return false;
    }
    if (!getModelLoader().saveRawMesh(meshEditAsset, meshEditPath, error)) {
        return false;
    }
    if (IsMMeshPath(meshEditPath)) {
        tmRenderer.getMeshCache().invalidate(meshEditPath);
    }
    meshEditDirty = false;
    fileBrowser.needsRefresh = true;
    return true;
}

bool Engine::saveDirtyMeshEditAssetForSceneSave() {
    if (!meshEditLoaded || !meshEditDirty) {
        return true;
    }
    std::string error;
    if (!saveMeshEditAsset(error)) {
        addConsoleMessage("Error: Failed to save edited RMesh before scene save: " + error,
                          ConsoleMessageType::Error);
        return false;
    }
    addConsoleMessage("Saved edited RMesh: " + meshEditPath, ConsoleMessageType::Success);
    return true;
}
#pragma endregion

#pragma region Material IO
void Engine::applyMaterialFileData(SceneObject& obj, const MaterialFileData& data) {
    obj.material = data.props;
    obj.albedoTexturePath = data.albedo;
    obj.overlayTexturePath = data.overlay;
    obj.normalMapPath = data.normal;
    obj.metallicRoughnessMapPath = data.metallicRoughness;
    obj.metallicMapPath = data.metallicMap;
    obj.roughnessMapPath = data.roughnessMap;
    obj.opacityMapPath = data.opacityMap;
    obj.emissiveMapPath = data.emissive;
    obj.occlusionMapPath = data.occlusion;
    obj.heightMapPath = data.height;
    obj.useOverlay = data.useOverlay;
    obj.shaderPackPath = data.shaderPack;
    obj.vertexShaderPath = data.vertexShader;
    obj.fragmentShaderPath = data.fragmentShader;
}

MaterialFileData Engine::materialFileDataFromObject(const SceneObject& obj) {
    MaterialFileData data;
    data.props = obj.material;
    data.albedo = obj.albedoTexturePath;
    data.overlay = obj.overlayTexturePath;
    data.normal = obj.normalMapPath;
    data.metallicRoughness = obj.metallicRoughnessMapPath;
    data.metallicMap = obj.metallicMapPath;
    data.roughnessMap = obj.roughnessMapPath;
    data.opacityMap = obj.opacityMapPath;
    data.emissive = obj.emissiveMapPath;
    data.occlusion = obj.occlusionMapPath;
    data.height = obj.heightMapPath;
    data.useOverlay = obj.useOverlay;
    data.shaderPack = obj.shaderPackPath;
    data.vertexShader = obj.vertexShaderPath;
    data.fragmentShader = obj.fragmentShaderPath;
    return data;
}

// materialPath holds whatever the assigning panel had: absolute from the Project panel,
// project-relative from older scenes. Both are resolved lexically - no stat - because this runs
// on every frame a material slider is dragged.
bool Engine::materialPathsMatch(const std::string& a, const std::string& b) const {
    if (a.empty() || b.empty()) return false;
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const fs::path pathA(a);
    const fs::path pathB(b);
    // Cheap reject first: most objects use some other material.
    if (lower(pathA.filename().string()) != lower(pathB.filename().string())) return false;
    auto key = [&](const fs::path& path) {
        fs::path resolved = path;
        if (!resolved.is_absolute() && projectManager.currentProject.isLoaded) {
            resolved = projectManager.currentProject.projectPath / resolved;
        }
        std::string text = resolved.lexically_normal().generic_string();
#ifdef _WIN32
        text = lower(text);
#endif
        return text;
    };
    return key(pathA) == key(pathB);
}

int Engine::propagateMaterialAsset(const std::string& materialPath, const MaterialFileData& data,
                                   int skipObjectId) {
    if (materialPath.empty()) return 0;
    int updated = 0;
    for (SceneObject& obj : sceneObjects) {
        if (obj.id == skipObjectId || !materialPathsMatch(obj.materialPath, materialPath)) continue;
        applyMaterialFileData(obj, data);
        ++updated;
    }
    if (updated > 0) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return updated;
}

int Engine::countMaterialAssetUsers(const std::string& materialPath) const {
    int users = 0;
    for (const SceneObject& obj : sceneObjects) {
        if (materialPathsMatch(obj.materialPath, materialPath)) ++users;
    }
    return users;
}

fs::path Engine::resolveMaterialAssetPath(const std::string& rawPath) const {
    if (rawPath.empty()) return {};
    const fs::path path(rawPath);
    if (path.is_absolute()) return path;
    // Saved scenes hold project-relative material paths. Opened as-is they resolve against the
    // working directory (the editor's build folder): reads fail, and a write could leave a
    // stray copy there. The working directory only wins for a file that exists there and not
    // in the project, like a material shipped under Resources.
    if (projectManager.currentProject.isLoaded && !projectManager.currentProject.projectPath.empty()) {
        const fs::path inProject = projectManager.currentProject.projectPath / path;
        std::error_code ec;
        if (fs::exists(inProject, ec) || !fs::exists(path, ec)) {
            return inProject.lexically_normal();
        }
    }
    return path;
}

void Engine::loadMaterialFromFile(SceneObject& obj) {
    if (obj.materialPath.empty()) return;
    try {
        MaterialFileData data;
        if (!readMaterialFile(resolveMaterialAssetPath(obj.materialPath).string(), data)) {
            addConsoleMessage("Failed to open material: " + obj.materialPath, ConsoleMessageType::Error);
            return;
        }
        applyMaterialFileData(obj, data);
        addConsoleMessage("Applied material: " + obj.materialPath, ConsoleMessageType::Success);
        projectManager.currentProject.hasUnsavedChanges = true;
    } catch (...) {
        addConsoleMessage("Failed to read material: " + obj.materialPath, ConsoleMessageType::Error);
    }
}

bool Engine::loadMaterialData(const std::string& path, MaterialProperties& props,
                              std::string& albedo, std::string& overlay,
                              std::string& normal, bool& useOverlay,
                              std::string* shaderPackOut,
                              std::string* vertexShaderOut,
                              std::string* fragmentShaderOut,
                              std::string* metallicRoughnessOut,
                              std::string* metallicMapOut,
                              std::string* roughnessMapOut,
                              std::string* opacityMapOut,
                              std::string* emissiveOut,
                              std::string* occlusionOut,
                              std::string* heightOut)
{
    MaterialFileData data;
    if (!readMaterialFile(resolveMaterialAssetPath(path).string(), data)) {
        return false;
    }
    props = data.props;
    albedo = data.albedo;
    overlay = data.overlay;
    normal = data.normal;
    useOverlay = data.useOverlay;
    if (shaderPackOut) *shaderPackOut = data.shaderPack;
    if (vertexShaderOut) *vertexShaderOut = data.vertexShader;
    if (fragmentShaderOut) *fragmentShaderOut = data.fragmentShader;
    if (metallicRoughnessOut) *metallicRoughnessOut = data.metallicRoughness;
    if (metallicMapOut) *metallicMapOut = data.metallicMap;
    if (roughnessMapOut) *roughnessMapOut = data.roughnessMap;
    if (opacityMapOut) *opacityMapOut = data.opacityMap;
    if (emissiveOut) *emissiveOut = data.emissive;
    if (occlusionOut) *occlusionOut = data.occlusion;
    if (heightOut) *heightOut = data.height;
    return true;
}

bool Engine::saveMaterialData(const std::string& path, const MaterialProperties& props,
                              const std::string& albedo, const std::string& overlay,
                              const std::string& normal, bool useOverlay,
                              const std::string& shaderPack,
                              const std::string& vertexShader,
                              const std::string& fragmentShader,
                              const std::string* metallicRoughness,
                              const std::string* metallicMap,
                              const std::string* roughnessMap,
                              const std::string* opacityMap,
                              const std::string* emissive,
                              const std::string* occlusion,
                              const std::string* height)
{
    const std::string filePath = resolveMaterialAssetPath(path).string();
    MaterialFileData data;
    readMaterialFile(filePath, data);
    data.props = props;
    data.albedo = albedo;
    data.overlay = overlay;
    data.normal = normal;
    data.useOverlay = useOverlay;
    data.shaderPack = shaderPack;
    data.vertexShader = vertexShader;
    data.fragmentShader = fragmentShader;
    if (metallicRoughness) data.metallicRoughness = *metallicRoughness;
    if (metallicMap) data.metallicMap = *metallicMap;
    if (roughnessMap) data.roughnessMap = *roughnessMap;
    if (opacityMap) data.opacityMap = *opacityMap;
    if (emissive) data.emissive = *emissive;
    if (occlusion) data.occlusion = *occlusion;
    if (height) data.height = *height;
    return writeMaterialFile(data, filePath);
}

int Engine::autoAssignProjectMaterialMaps() {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("Open a project first.", ConsoleMessageType::Warning);
        return 0;
    }
    const fs::path projectRoot(projectManager.currentProject.projectPath);
    const fs::path assetsRoot = projectRoot / "Assets";

    std::error_code ec;
    if (!fs::is_directory(assetsRoot, ec)) {
        addConsoleMessage("No Assets folder under " + projectRoot.string(),
                          ConsoleMessageType::Warning);
        return 0;
    }

    std::vector<fs::path> materials;
    for (const auto& entry : fs::recursive_directory_iterator(
             assetsRoot, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) continue;
        if (entry.path().extension() == ".mat") materials.push_back(entry.path());
    }

    if (materials.empty()) {
        addConsoleMessage("No .mat files found under " + assetsRoot.string(),
                          ConsoleMessageType::Info);
        return 0;
    }

    ModelTextureResolver resolver(assetsRoot, projectRoot);
    int changedCount = 0;
    int slotsFilled = 0;

    for (const fs::path& matPath : materials) {
        MaterialFileData data;
        if (!readMaterialFile(matPath.string(), data)) continue;

        const std::string matStem = matPath.stem().string();
        fs::path searchDir = matPath.parent_path();
        if (!data.albedo.empty()) {
            const fs::path albedo(data.albedo);
            const fs::path albedoAbs = albedo.is_absolute() ? albedo : (projectRoot / albedo);
            if (fs::is_regular_file(albedoAbs, ec)) searchDir = albedoAbs.parent_path();
        }
        const fs::path probe = searchDir / matStem;

        struct Slot { std::string* field; ModelTextureResolver::Role role; };
        const Slot slots[] = {
            {&data.roughnessMap, ModelTextureResolver::Role::Roughness},
            {&data.metallicMap,  ModelTextureResolver::Role::Metallic},
            {&data.opacityMap,   ModelTextureResolver::Role::Opacity},
            {&data.emissive,     ModelTextureResolver::Role::Emissive},
            {&data.occlusion,    ModelTextureResolver::Role::Occlusion},
        };

        bool dirty = false;
        for (const Slot& slot : slots) {
            if (!slot.field->empty()) continue;
            std::string found = resolver.resolve("", probe, slot.role, matStem);
            if (found.empty()) continue;
            *slot.field = found;
            dirty = true;
            ++slotsFilled;
        }

        if (!data.metallicMap.empty() || !data.roughnessMap.empty()) {
            if (!data.props.pbrEnabled) { data.props.pbrEnabled = true; dirty = true; }
        }
        if (!data.emissive.empty() && data.props.emissionStrength <= 0.0f) {
            data.props.emissionStrength = 1.5f;
            dirty = true;
        }

        if (dirty && writeMaterialFile(data, matPath.string())) {
            ++changedCount;
            addConsoleMessage("Updated " + matStem, ConsoleMessageType::Info);
        }
    }

    addConsoleMessage("Auto-assign: " + std::to_string(slotsFilled) + " map(s) filled across " +
                          std::to_string(changedCount) + " of " +
                          std::to_string(materials.size()) + " material(s). Reload the scene "
                          "to see them.",
                      changedCount ? ConsoleMessageType::Success : ConsoleMessageType::Info);
    return changedCount;
}

bool Engine::applyTextureAssetToObject(SceneObject& obj, const fs::path& texturePath) {
    if (texturePath.empty()) return false;

    std::error_code ec;
    fs::directory_entry entry(texturePath, ec);
    if (ec || fileBrowser.getFileCategory(entry) != FileCategory::Texture) {
        return false;
    }

    std::string stem = texturePath.stem().string();
    std::string lowerStem = stem;
    std::transform(lowerStem.begin(), lowerStem.end(), lowerStem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    enum class TextureSlot { Albedo, Normal, Roughness, Metallic };
    TextureSlot slot = TextureSlot::Albedo;
    if (lowerStem.find("_normal") != std::string::npos ||
        lowerStem.find("-normal") != std::string::npos ||
        lowerStem.find(" normal") != std::string::npos ||
        lowerStem.find("_nrm") != std::string::npos) {
        slot = TextureSlot::Normal;
    } else if (lowerStem.find("_roughness") != std::string::npos ||
               lowerStem.find("-roughness") != std::string::npos ||
               lowerStem.find("_rough") != std::string::npos) {
        slot = TextureSlot::Roughness;
    } else if (lowerStem.find("_metallic") != std::string::npos ||
               lowerStem.find("-metallic") != std::string::npos ||
               lowerStem.find("_metalness") != std::string::npos) {
        slot = TextureSlot::Metallic;
    }

    const std::string textureString = texturePath.string();
    if ((slot == TextureSlot::Albedo && obj.albedoTexturePath == textureString) ||
        (slot == TextureSlot::Normal && obj.normalMapPath == textureString)) {
        return false;
    }

    fs::path materialsDir = projectManager.currentProject.assetsPath / "Materials";
    fs::create_directories(materialsDir, ec);
    if (ec) {
        addConsoleMessage("Failed to create Materials folder: " + ec.message(), ConsoleMessageType::Error);
        return false;
    }

    fs::path materialPath = obj.materialPath.empty()
        ? materialsDir / (obj.name + "_" + texturePath.stem().string() + ".mat")
        : fs::path(obj.materialPath);

    if (!obj.materialPath.empty()) {
        const bool sameGeneratedMaterial =
            materialPath.parent_path() == materialsDir &&
            materialPath.stem().string().find(obj.name + "_") == 0;
        if (!sameGeneratedMaterial) {
            fs::path instancePath = materialsDir / (obj.name + "_" + texturePath.stem().string() + ".mat");
            int suffix = 1;
            while (fs::exists(instancePath) && instancePath != materialPath) {
                instancePath = materialsDir / (obj.name + "_" + texturePath.stem().string() + "_" + std::to_string(suffix++) + ".mat");
            }
            materialPath = instancePath;
        }
    }

    if (slot == TextureSlot::Albedo) {
        obj.albedoTexturePath = textureString;
    } else if (slot == TextureSlot::Normal) {
        obj.normalMapPath = textureString;
        obj.material.normalMapIntensity = std::max(0.01f, obj.material.normalMapIntensity);
    } else if (slot == TextureSlot::Roughness) {
        obj.material.shininess = 16.0f;
    } else if (slot == TextureSlot::Metallic) {
        obj.material.specularStrength = 1.0f;
    }

    obj.materialPath = materialPath.string();
    if (!saveMaterialData(obj.materialPath, obj.material, obj.albedoTexturePath, obj.overlayTexturePath,
                          obj.normalMapPath, obj.useOverlay, obj.shaderPackPath,
                          obj.vertexShaderPath, obj.fragmentShaderPath)) {
        addConsoleMessage("Failed to save generated material: " + obj.materialPath, ConsoleMessageType::Error);
        return false;
    }

    projectManager.currentProject.hasUnsavedChanges = true;
    addConsoleMessage("Applied texture to " + obj.name + " using material " + materialPath.filename().string(),
                      ConsoleMessageType::Success);
    return true;
}

void Engine::saveMaterialToFile(const SceneObject& obj) {
    if (obj.materialPath.empty()) {
        addConsoleMessage("Material path is empty", ConsoleMessageType::Warning);
        return;
    }
    try {
        const MaterialFileData data = materialFileDataFromObject(obj);
        const std::string filePath = resolveMaterialAssetPath(obj.materialPath).string();
        if (!writeMaterialFile(data, filePath)) {
            addConsoleMessage("Failed to open material for writing: " + filePath, ConsoleMessageType::Error);
            return;
        }
        addConsoleMessage("Saved material: " + obj.materialPath, ConsoleMessageType::Success);
    } catch (...) {
        addConsoleMessage("Failed to save material: " + obj.materialPath, ConsoleMessageType::Error);
    }
}
#pragma endregion

#pragma region Editor Shortcuts
void Engine::handleKeyboardShortcuts() {
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
        viewportFullscreen = !viewportFullscreen;
    }

    if (playerMode) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && gameViewCursorLocked) {
            gameViewCursorLocked = false;
        }
        return;
    }

    const bool ctrlDown = ImGui::GetIO().KeyCtrl;
    const bool shiftDown = ImGui::GetIO().KeyShift;

    if (ctrlDown && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (projectManager.currentProject.isLoaded) {
            if (shiftDown) {
                showSaveSceneAsDialog = true;
                strncpy(saveSceneAsName,
                        projectManager.currentProject.currentSceneName.c_str(),
                        sizeof(saveSceneAsName) - 1);
                saveSceneAsName[sizeof(saveSceneAsName) - 1] = '\0';
            } else {
                saveCurrentScene();
            }
        }
    }

    if (ctrlDown && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        if (projectManager.currentProject.isLoaded) {
            if (shiftDown) {
                togglePause();
            } else {
                ImGui::ClearActiveID();
                togglePlayMode();
            }
        }
    }

    if (ctrlDown && !shiftDown) {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            ImGui::SetWindowFocus(Loc::WindowRef("Viewport"));
        } else if (ImGui::IsKeyPressed(ImGuiKey_2, false)) {
            showGameViewport = true;
            ImGui::SetWindowFocus(Loc::WindowRef("Game Viewport"));
        } else if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
            showInspector = true;
            ImGui::SetWindowFocus(Loc::WindowRef("Inspector"));
        } else if (ImGui::IsKeyPressed(ImGuiKey_4, false)) {
            showHierarchy = true;
            ImGui::SetWindowFocus(Loc::WindowRef("Hierarchy"));
        } else if (ImGui::IsKeyPressed(ImGuiKey_5, false)) {
            showFileBrowser = true;
            ImGui::SetWindowFocus(Loc::WindowRef("Project"));
        }
    }

    if (ctrlDown && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        if (projectManager.currentProject.isLoaded) {
            showNewSceneDialog = true;
            memset(newSceneName, 0, sizeof(newSceneName));
        }
    }

    const bool textInputFocused = ImGui::GetIO().WantTextInput;
    const bool sceneShortcutsActive = !textInputFocused && !fileBrowserPanelFocused;
    if (sceneShortcutsActive && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        deleteSelected();
    }

    if (sceneShortcutsActive && ctrlDown && !shiftDown) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            copySelected();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            pasteClipboard();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            selectAllObjects();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
            duplicateSelected();
        }
    }

    bool cameraActive = cursorLocked || (viewportController.isViewportFocused() && cursorLocked);
    if (!isPlaying && gameViewCursorLocked) {
        // Prevent edit-mode freelook from conflicting with game view capture
        gameViewCursorLocked = false;
    }
    if (!cameraActive && !textInputFocused) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q)) mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_W)) mCurrentGizmoOperation = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) mCurrentGizmoOperation = ImGuizmo::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) mCurrentGizmoOperation = ImGuizmo::BOUNDS;
        if (ImGui::IsKeyPressed(ImGuiKey_T)) mCurrentGizmoOperation = ImGuizmo::UNIVERSAL;

        if (ImGui::IsKeyPressed(ImGuiKey_U)) {
            mCurrentGizmoMode = (mCurrentGizmoMode == ImGuizmo::LOCAL) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        }
    }

    if (!textInputFocused && !ctrlDown && !meshEditMode && ImGui::IsKeyPressed(ImGuiKey_3)) {
        collisionWireframe = !collisionWireframe;
        addConsoleMessage(std::string("Selection collider bounds ") + (collisionWireframe ? "enabled" : "disabled"),
                          ConsoleMessageType::Info);
    }

    static bool snapPressed = false;
    static bool snapHeldByCtrl = false;
    static bool snapStateBeforeCtrl = false;

    if (!snapHeldByCtrl && ctrlDown) {
        snapStateBeforeCtrl = useSnap;
        snapHeldByCtrl = true;
        useSnap = true;
    } else if (snapHeldByCtrl && !ctrlDown) {
        useSnap = snapStateBeforeCtrl;
        snapHeldByCtrl = false;
    }

    if (!cameraActive && !textInputFocused && !ctrlDown) {
        if (ImGui::IsKeyPressed(ImGuiKey_Y) && !snapPressed) {
            useSnap = !useSnap;
            snapPressed = true;
        }
    }
    if (ImGui::IsKeyReleased(ImGuiKey_Y)) {
        snapPressed = false;
    }

    if (ctrlDown && !shiftDown && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        undo();
    }
    if (ctrlDown &&
        (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
         (shiftDown && ImGui::IsKeyPressed(ImGuiKey_Z, false)))) {
        redo();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && gameViewCursorLocked) {
        gameViewCursorLocked = false;
    }
}

void Engine::focusViewportOnSelection() {
    SceneObject* obj = getSelectedObject();
    if (!obj) return;

    const float scaleRadius = glm::length(glm::abs(obj->scale)) * 0.5f;
    const float focusDistance = std::clamp(std::max(1.25f, scaleRadius * 2.8f), 2.0f, 80.0f);
    glm::vec3 viewDir = camera.front;
    if (!std::isfinite(viewDir.x) || glm::length(viewDir) < 1e-4f) {
        viewDir = glm::vec3(0.0f, 0.0f, -1.0f);
    } else {
        viewDir = glm::normalize(viewDir);
    }

    viewportFocusActive = true;
    viewportFocusStartTime = glfwGetTime();
    viewportFocusStartPosition = camera.position;
    viewportFocusTargetPosition = obj->position - viewDir * focusDistance;
    viewportFocusStartFront = camera.front;
    viewportFocusTargetFront = glm::normalize(obj->position - viewportFocusTargetPosition);
    camera.velocity = glm::vec3(0.0f);
}
#pragma endregion

#pragma region Runtime Updates
void Engine::rebuildModuObjAssetIndex() {
    moduObjAssetIndex.clear();
    moduObjAssetCache.clear();
    const fs::path root = projectManager.currentProject.projectPath;
    moduObjAssetIndexRoot = root.string();
    if (root.empty()) return;

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        if (it->path().extension() != ModuObj::kAssetExtension) continue;

        ModuObj::Asset asset;
        std::string error;
        if (!ModuObj::LoadAsset(it->path(), asset, error)) {
            addConsoleMessage("[Network] Skipping unreadable ModuOBJ " +
                                  it->path().filename().string() + ": " + error,
                              ConsoleMessageType::Warning);
            continue;
        }
        moduObjAssetIndex[asset.assetId] = it->path();
    }
}

void Engine::updateNetworking(float delta) {
    const bool wantRunning = isPlaying || specMode || testMode;

    if (!wantRunning) {
        if (networkSessionActive) {
            networkSession.shutdown();
            networkSessionActive = false;
            addConsoleMessage("[Network] Session stopped.", ConsoleMessageType::Info);
        }
        return;
    }

    if (!networkSessionActive) {
        const int managerId = FindActiveNetworkManager(sceneObjects);
        if (managerId < 0) {
            const int managerCount = CountNetworkManagers(sceneObjects);
            if (managerCount > 1) {
                addConsoleMessage("[Network] " + std::to_string(managerCount) +
                                      " active Network Managers in the scene; networking disabled. "
                                      "Keep exactly one.",
                                  ConsoleMessageType::Error);
                networkSessionActive = true;
                networkSession.shutdown();
            }
            return;
        }

        const SceneObject* manager = findObjectById(managerId);
        if (!manager) return;
        const NetworkManagerComponent cfgSource = manager->networkManager;

        if (moduObjAssetIndexRoot != projectManager.currentProject.projectPath.string()) {
            rebuildModuObjAssetIndex();
        }

        Net::SessionConfig config;
        config.appId = cfgSource.appId;
        config.appVersion = cfgSource.appVersion;
        config.nickname = cfgSource.nickname;
        config.region = cfgSource.region;
        config.offlineMode = cfgSource.offlineMode;
        config.maxPlayers = cfgSource.maxPlayers;
        config.sendRateHz = cfgSource.sendRateHz;
        config.serializationRateHz = cfgSource.serializationRateHz;
        config.maxOutboundBytesPerSecond = cfgSource.maxOutboundBytesPerSecond;

        Net::SceneBridge bridge;
        bridge.sceneObjects = &sceneObjects;
        bridge.nextObjectId = &nextObjectId;
        bridge.resolveAsset = [this](const std::string& assetId, std::string& outError)
                                  -> const ModuObj::Asset* {
            auto it = moduObjAssetIndex.find(assetId);
            if (it == moduObjAssetIndex.end()) {
                outError = "no ModuOBJ with asset id " + assetId + " in this project";
                return nullptr;
            }
            return moduObjAssetCache.get(it->second, outError);
        };


        // A networked spawn appends straight into sceneObjects through the
        // SceneBridge, which has no way to tell the engine it happened. Everything
        // the engine caches about the scene is then describing the scene as it was:
        //
        //   runtimeScriptBindings - the list updateScripts walks. A spawned object is
        //                           absent from it, so its scripts never tick, and
        //                           because Begin() fires on a script's first tick
        //                           (mod->beginCalledObjects), it never begins either.
        //                           That is the "it spawned but does nothing" case.
        //   sceneObjectIndexById  - id lookups miss the new objects entirely.
        //   hierarchy transforms  - localPosition/localRotation are uninitialised
        //                           until a hierarchy pass runs, so a child spawned
        //                           under a parent sits at the wrong offset.
        //
        // Rebuilding is cheap next to a spawn and only happens per spawned object, so
        // this marks everything dirty rather than trying to patch the caches in place.
        auto refreshAfterSceneMutation = [this]() {
            ScriptSceneMutationScope mutation(this);
            markRuntimeScriptBindingsDirty();
            sceneObjectIndexById.clear();
            sceneObjectIndexData = nullptr;
            sceneObjectIndexCount = 0;
            updateHierarchyWorldTransforms();
        };
        networkSession.onObjectSpawned = [refreshAfterSceneMutation](Net::NetworkId) {
            refreshAfterSceneMutation();
        };
        // Despawn removes objects from the same vector, so stale bindings would point
        // at ids that are gone - harmless today because updateScripts skips unknown
        // ids, but it leaves the index wrong for everything that follows.
        networkSession.onObjectDespawned = [refreshAfterSceneMutation](Net::NetworkId) {
            refreshAfterSceneMutation();
        };
        networkSession.onLog = [this](Net::LogLevel level, const std::string& message) {
            ConsoleMessageType type = ConsoleMessageType::Info;
            switch (level) {
                case Net::LogLevel::Warning: type = ConsoleMessageType::Warning; break;
                case Net::LogLevel::Error:   type = ConsoleMessageType::Error;   break;
                case Net::LogLevel::Success: type = ConsoleMessageType::Success; break;
                default: break;
            }
            addConsoleMessage(message, type);
        };

        if (!networkSession.start(config, bridge)) {
            addConsoleMessage("[Network] Failed to start session: " +
                                  networkSession.lastError().message,
                              ConsoleMessageType::Error);
            networkSessionActive = true;
            return;
        }
        networkSessionActive = true;

        if (!cfgSource.offlineMode && cfgSource.appId.empty()) {
            addConsoleMessage("[Network] No Photon App ID set on the Network Manager; "
                              "running an offline session instead.",
                              ConsoleMessageType::Warning);
        }

        if (cfgSource.autoConnect) {
            networkSession.connect();
            if (cfgSource.autoJoinLobby) networkSession.joinLobby();
            if (!cfgSource.defaultRoomName.empty()) {
                Net::RoomOptions options;
                options.maxPlayers = cfgSource.maxPlayers;
                networkSession.joinOrCreateRoom(cfgSource.defaultRoomName, options);
            }
        }
    }

    networkSession.tick(delta);
}

void Engine::dispatchPhysicsCollisionEvents(float delta) {
    physicsCollisionEvents.clear();
    physics->drainCollisionEvents(physicsCollisionEvents);
    if (physicsCollisionEvents.empty()) return;

    auto dispatchToObject = [&](int objectId, int otherId, PhysicsCollisionPhase phase) {
        SceneObject* object = findObjectById(objectId);
        SceneObject* other = findObjectById(otherId);
        if (!object || !other) return;

        const size_t scriptCount = object->scripts.size();
        for (size_t scriptIndex = 0; scriptIndex < scriptCount; ++scriptIndex) {
            object = findObjectById(objectId);
            other = findObjectById(otherId);
            if (!object || !other || scriptIndex >= object->scripts.size()) break;
            ScriptComponent& script = object->scripts[scriptIndex];
            if (!script.enabled || script.path.empty() || script.language == ScriptLanguage::CSharp) continue;

            fs::path binary;
            if (!script.lastBinaryPath.empty()) {
                std::error_code ec;
                if (fs::exists(script.lastBinaryPath, ec) && !ec) {
                    binary = script.lastBinaryPath;
                    script.lastBinaryVerified = true;
                }
            }
            if (binary.empty()) {
                binary = resolveScriptBinary(script.path);
                if (binary.empty()) continue;
                script.lastBinaryPath = binary.string();
                script.lastBinaryVerified = true;
            }

            ScriptContext ctx;
            ctx.engine = this;
            ctx.object = object;
            ctx.script = &script;
            scriptRuntime.dispatchCollision(binary, ctx, other, phase, delta);
        }
    };

    for (size_t i = 0; i < physicsCollisionEvents.size(); ++i) {
        const PhysicsCollisionEvent event = physicsCollisionEvents[i];
        dispatchToObject(event.objectAId, event.objectBId, event.phase);
        dispatchToObject(event.objectBId, event.objectAId, event.phase);
    }
    physicsCollisionEvents.clear();
}

void Engine::updateScripts(float delta) {
    MODU_PROFILE_SCOPE("Script Update", ProfilerSampleCategory::Script);
#if !MODULARITY_RUNTIME_ONLY
    static bool runtimeTraceWasActive = false;
    static int runtimeTraceFramesRemaining = 0;
    const bool runtimeActive = isPlaying || specMode || testMode;
    if (runtimeActive && !runtimeTraceWasActive) {
        runtimeTraceFramesRemaining = 3;
        std::cerr << "[RuntimeTrace] Enter runtime mode play=" << (isPlaying ? 1 : 0)
                  << " spec=" << (specMode ? 1 : 0)
                  << " test=" << (testMode ? 1 : 0) << std::endl;
    } else if (!runtimeActive && runtimeTraceWasActive) {
        std::cerr << "[RuntimeTrace] Exit runtime mode" << std::endl;
    }
    runtimeTraceWasActive = runtimeActive;
#endif

    if (sceneObjects.empty()) return;
    if (runtimeScriptBindingsCachedVersion != runtimeScriptBindingsVersion) {
        rebuildRuntimeScriptBindings();
    }
    if (runtimeScriptBindings.empty()) return;
    refreshSceneObjectIndexCache();

#if MODULARITY_RUNTIME_ONLY
    constexpr bool traceScripts = false;
#else
    const bool traceScripts = runtimeTraceFramesRemaining > 0;
#endif
    auto scriptLanguageLabel = [](ScriptLanguage language) {
        switch (language) {
            case ScriptLanguage::Cpp: return "Cpp";
            case ScriptLanguage::CSharp: return "CSharp";
            case ScriptLanguage::C: return "C";
            default: return "Unknown";
        }
    };
    if (traceScripts) {
        std::cerr << "[RuntimeTrace] updateScripts begin frame=" << ImGui::GetFrameCount()
                  << " bindings=" << runtimeScriptBindings.size() << std::endl;
    }

    using ManagedClock = std::chrono::steady_clock;
    const ManagedScriptRuntime::GcStats gcStartStats = managedRuntime.getGcStats();
    ManagedScriptRuntime::GcStats gcEndStats = gcStartStats;
    double gcMs = 0.0;

    for (const RuntimeScriptBinding binding : runtimeScriptBindings) {
        SceneObject* currentObject = findObjectById(binding.objectId);
        if (!currentObject) continue;
        SceneObject& obj = *currentObject;
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        if (binding.scriptIndex >= obj.scripts.size()) continue;
        ScriptComponent& sc = obj.scripts[binding.scriptIndex];
        if (!sc.enabled) continue;
        if (sc.path.empty()) continue;

        ScriptContext ctx;
        ctx.engine = this;
        ctx.object = &obj;
        ctx.script = &sc;
        if (sc.language == ScriptLanguage::CSharp) {
            fs::path assembly;
            if (!sc.lastBinaryPath.empty()) {
                if (sc.lastBinaryVerified) {
                    assembly = fs::path(sc.lastBinaryPath);
                } else {
                    fs::path cachedAssembly = sc.lastBinaryPath;
                    if (fs::exists(cachedAssembly)) {
                        assembly = std::move(cachedAssembly);
                        sc.lastBinaryVerified = true;
                    }
                }
            }
            if (assembly.empty()) {
                assembly = resolveManagedAssembly(sc.path);
                sc.lastBinaryPath = assembly.string();
                std::error_code ec;
                sc.lastBinaryVerified = !assembly.empty() && fs::exists(assembly, ec) && !ec;
            }
            if (assembly.empty()) continue;
            if (!sc.lastBinaryVerified) {
                std::error_code ec;
                if (!fs::exists(assembly, ec) || ec) continue;
                sc.lastBinaryVerified = true;
            }
            if (traceScripts) {
                std::cerr << "[RuntimeTrace] script start obj=\"" << obj.name
                          << "\" id=" << obj.id
                          << " slot=" << binding.scriptIndex
                          << " lang=" << scriptLanguageLabel(sc.language)
                          << " type=\"" << sc.managedType
                          << "\" binary=\"" << assembly.string() << "\""
                          << std::endl;
            }
            const ManagedScriptRuntime::GcStats beforeManagedCall = managedRuntime.getGcStats();
            const auto managedCallStart = ManagedClock::now();
            managedRuntime.tickModule(assembly, sc.managedType, ctx, delta, specMode, testMode);
            if (traceScripts) {
                std::cerr << "[RuntimeTrace] script end obj=\"" << (ctx.object ? ctx.object->name : "<deleted>")
                          << "\" id=" << binding.objectId
                          << " slot=" << binding.scriptIndex << std::endl;
            }
            const ManagedScriptRuntime::GcStats afterManagedCall = managedRuntime.getGcStats();
            gcEndStats = afterManagedCall;
            if (beforeManagedCall.available && afterManagedCall.available) {
                bool collected = false;
                for (size_t generation = 0; generation < beforeManagedCall.collectionCounts.size(); ++generation) {
                    if (afterManagedCall.collectionCounts[generation] > beforeManagedCall.collectionCounts[generation]) {
                        collected = true;
                        break;
                    }
                }
                if (collected) {
                    gcMs += std::chrono::duration<double, std::milli>(
                        ManagedClock::now() - managedCallStart).count();
                }
            }
        } else {
            fs::path binary;
            const bool attachedBinaryDirectly = IsNativeBinaryPath(fs::path(sc.path));
            if (!attachedBinaryDirectly && (!sc.lastBinaryVerified || sc.lastBinaryPath.empty())) {
                fs::path resolvedBinary = resolveScriptBinary(sc.path);
                if (!resolvedBinary.empty()) {
                    binary = resolvedBinary;
                    sc.lastBinaryPath = resolvedBinary.string();
                }
            }
            if (binary.empty() && !sc.lastBinaryPath.empty()) {
                if (sc.lastBinaryVerified) {
                    binary = fs::path(sc.lastBinaryPath);
                } else {
                    fs::path cachedBinary = sc.lastBinaryPath;
                    if (fs::exists(cachedBinary)) {
                        binary = std::move(cachedBinary);
                        sc.lastBinaryVerified = true;
                    }
                }
            }
            if (binary.empty()) {
                binary = resolveScriptBinary(sc.path);
                sc.lastBinaryPath = binary.string();
            }
            bool hasBinary = !binary.empty();
            if (!hasBinary || !sc.lastBinaryVerified) {
                std::error_code ec;
                hasBinary = !binary.empty() && fs::exists(binary, ec) && !ec;
                sc.lastBinaryVerified = hasBinary;
            }
            if (!hasBinary) {
                fs::path scriptPath(sc.path);
                std::string missingKey = scriptPath.lexically_normal().string();
                if (nativeScriptMissingLogged.insert(missingKey).second) {
                    std::cerr << "[Script] Native script binary missing for '" << sc.path
                              << "'. Compile scripts before running/exporting.\n";
                    addConsoleMessage(
                        "Native script binary missing for '" + sc.path +
                        "'. Compile scripts before running/exporting.",
                        ConsoleMessageType::Warning);
                }
                continue;
            }

            std::string binaryKey = binary.lexically_normal().string();
            nativeScriptMissingLogged.erase(fs::path(sc.path).lexically_normal().string());
            if (traceScripts) {
                std::cerr << "[RuntimeTrace] script start obj=\"" << obj.name
                          << "\" id=" << obj.id
                          << " slot=" << binding.scriptIndex
                          << " lang=" << scriptLanguageLabel(sc.language)
                          << " path=\"" << sc.path
                          << "\" binary=\"" << binary.string() << "\""
                          << std::endl;
            }
            scriptRuntime.tickModule(binary, ctx, delta, specMode, testMode);
            if (traceScripts) {
                std::cerr << "[RuntimeTrace] script end obj=\"" << (ctx.object ? ctx.object->name : "<deleted>")
                          << "\" id=" << binding.objectId
                          << " slot=" << binding.scriptIndex << std::endl;
            }
            const std::string& runtimeError = scriptRuntime.getLastError();
            if (!runtimeError.empty()) {
                std::string errorKey = binaryKey + "|" + runtimeError;
                if (nativeScriptLoadErrorLogged.insert(errorKey).second) {
                    std::cerr << "[Script] Failed to load native script '" << binary.string()
                              << "': " << runtimeError << "\n";
                    addConsoleMessage(
                        "Failed to load native script '" + binary.string() +
                        "': " + runtimeError,
                        ConsoleMessageType::Error);
                }
                if (ctx.script) requestRecompileForScriptLoadFailure(fs::path(ctx.script->path),
                                                     scriptRuntime.getLastFailure());
            }
        }
    }

    if (traceScripts) {
        std::cerr << "[RuntimeTrace] updateScripts end frame=" << ImGui::GetFrameCount() << std::endl;
#if !MODULARITY_RUNTIME_ONLY
        --runtimeTraceFramesRemaining;
#endif
    }

    if (gcStartStats.available || gcEndStats.available) {
        if (!gcEndStats.available) {
            gcEndStats = managedRuntime.getGcStats();
        }
        std::array<uint32_t, 3> gcDelta = { 0, 0, 0 };
        for (size_t generation = 0; generation < gcDelta.size(); ++generation) {
            const uint32_t before = gcStartStats.collectionCounts[generation];
            const uint32_t after = gcEndStats.collectionCounts[generation];
            gcDelta[generation] = (after >= before) ? (after - before) : 0u;
        }
        const int64_t heapDeltaBytes =
            static_cast<int64_t>(gcEndStats.usedBytes) - static_cast<int64_t>(gcStartStats.usedBytes);
        Profiler::instance().setCurrentFrameGcMetrics(gcMs,
                                                      heapDeltaBytes,
                                                      gcEndStats.usedBytes,
                                                      gcEndStats.heapBytes,
                                                      gcEndStats.collectionCounts,
                                                      gcDelta);

        const uint32_t totalCollections = gcDelta[0] + gcDelta[1] + gcDelta[2];
        if (totalCollections > 0 || gcMs > 0.0001) {
            Profiler::instance().addSyntheticSample("GC",
                                                    ProfilerSampleCategory::GC,
                                                    gcMs,
                                                    std::max<uint32_t>(1u, totalCollections),
                                                    0,
                                                    heapDeltaBytes,
                                                    gcEndStats.usedBytes);
        }
    }
}

void Engine::queueAutoCompile(const fs::path& scriptPath, const fs::file_time_type& sourceTime) {
    std::error_code ec;
    fs::path scriptAbs = fs::absolute(scriptPath, ec);
    if (ec) scriptAbs = scriptPath;
    std::string key = scriptAbs.lexically_normal().string();
    if (!autoCompileQueued.insert(key).second) return;

    autoCompileQueue.push_back(scriptAbs);
    scriptLastAutoCompileTime[key] = sourceTime;
}

void Engine::queueScriptCompile(const fs::path& scriptPath) {
    if (scriptPath.empty()) return;
    std::error_code ec;
    fs::path normalized = fs::absolute(scriptPath, ec);
    if (ec) normalized = scriptPath;
    normalized = normalized.lexically_normal();

    ScriptCompileQueueItem item;
    item.scriptPath = normalized;
    item.displayLabel = normalized.filename().empty() ? normalized.string() : normalized.filename().string();
    if (item.displayLabel.empty()) {
        item.displayLabel = "Script";
    }
    const std::string ext = normalized.extension().string();
    item.managed = (ext == ".cs" || ext == ".csproj");

    const std::string key = item.managed ? std::string("managed:") + normalized.string() : normalized.string();
    if (!compileRequestKeys.insert(key).second) {
        return;
    }

    if (compileRequestQueue.empty() && !compileInProgress) {
        compileHistory.clear();
        compileBatchTotal = 0;
        compileBatchCompleted = 0;
        compileCompletionStart = 0.0;
        compileUiToast = nextCompileFromAuto;
        showCompilePopup = true;
        compilePopupHideTime = 0.0;
        compilePopupOpened = false;
        playCompileStartSound();
    } else if (!nextCompileFromAuto && compileUiToast) {
        compileUiToast = false;
        compilePopupOpened = false;
        showCompilePopup = true;
    }

    compileRequestQueue.push_back(std::move(item));
    ++compileBatchTotal;
}

void Engine::queueScriptCompileBatch(const std::vector<fs::path>& scriptPaths) {
    for (const auto& scriptPath : scriptPaths) {
        queueScriptCompile(scriptPath);
    }

    if (!compileInProgress) {
        compileBatchCompleted = std::min(compileBatchCompleted, std::max(0, compileBatchTotal - 1));
        startQueuedCompileJobs();
    } else {
        showCompilePopup = true;
        compilePopupOpened = false;
        startQueuedCompileJobs();
    }
}

namespace {
    int compileMaxParallelJobs() {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1u;

        // Leave a thread for the editor. Script builds run as async workers while the render thread
        // keeps drawing, so filling every core with compilers starves the UI - unnoticeable at 24
        // threads, total stall at 2.
        unsigned jobs = (hw > 1u) ? (hw - 1u) : 1u;

        // A single script TU pulls in the whole script API surface and can peak near a gigabyte in
        // the compiler. Past roughly one worker per GB of RAM the extra parallelism is paid for in
        // swap, which on a low-memory machine is far worse than compiling serially.
        const unsigned long long memMB = Modularity::HardwareProfile::SystemMemoryMB();
        if (memMB > 0) {
            const unsigned long long byMemory = memMB / 1024ull;
            jobs = static_cast<unsigned>(
                std::min<unsigned long long>(jobs, byMemory > 0ull ? byMemory : 1ull));
        }

        return static_cast<int>(jobs > 0u ? jobs : 1u);
    }
}

bool Engine::isScriptScanExcludedDir(const std::string& name, bool atScanRoot) {
    if (!name.empty() && name[0] == '.') return true;
    if (!atScanRoot) return false;
    return name == "Library" || name == "Cache" || name == "Builds" ||
           name == "Build" || name == "Packages" || name == "ProjectSettings" ||
           name == "ProjectUserSettings" || name == "BuildProfiles";
}

void Engine::startQueuedCompileJobs() {
    while (static_cast<int>(compileWorkers.size()) < compileMaxParallelJobs() &&
           !compileRequestQueue.empty()) {
        if (compileRequestQueue.front().managed && !compileWorkers.empty()) {
            break;
        }

        const ScriptCompileQueueItem next = compileRequestQueue.front();
        compileRequestQueue.pop_front();
        const std::string key = next.managed ? std::string("managed:") + next.scriptPath.string()
                                             : next.scriptPath.string();
        compileRequestKeys.erase(key);
        compileCurrentLabel = next.displayLabel;
        compileCurrentPath = next.scriptPath;
        compileCurrentManaged = next.managed;

        if (next.managed) {
            compileManagedScripts();
            break;
        }
        compileScriptFile(next.scriptPath);
    }
}

void Engine::playCompileStartSound() {
    playEditorSound(EditorSoundCue::CompileStart, 0.95f);
}

bool Engine::isEditorFeedbackSoundEnabled(EditorFeedbackSoundCategory category) const {
    if (category == EditorFeedbackSoundCategory::Boot) {
        return true;
    }
    if (!feedbackSoundsEnabled) {
        return false;
    }
    switch (category) {
        case EditorFeedbackSoundCategory::Click:
            return feedbackClickSoundsEnabled;
        case EditorFeedbackSoundCategory::Error:
            return feedbackErrorSoundsEnabled;
        case EditorFeedbackSoundCategory::Other:
            return feedbackOtherSoundsEnabled;
        case EditorFeedbackSoundCategory::Button:
            return feedbackButtonSoundsEnabled;
        case EditorFeedbackSoundCategory::Toggle:
            return feedbackToggleSoundsEnabled;
        case EditorFeedbackSoundCategory::Slide:
            return feedbackSlideSoundsEnabled;
        case EditorFeedbackSoundCategory::Transition:
            return feedbackTransitionSoundsEnabled;
        case EditorFeedbackSoundCategory::Hover:
            return feedbackHoverSoundsEnabled;
        case EditorFeedbackSoundCategory::Boot:
        default:
            return true;
    }
}

bool Engine::playEditorFeedbackOneShot(const std::string& path,
                                       float volume,
                                       EditorFeedbackSoundCategory category) {
    if (!audio.isReady() || !isEditorFeedbackSoundEnabled(category)) {
        return false;
    }
    audio.playOneShot(path, volume);
    return true;
}

Engine::EditorFeedbackSoundCategory Engine::feedbackCategoryForCue(EditorSoundCue cue) {
    switch (cue) {
        case EditorSoundCue::Tick:
        case EditorSoundCue::SectionSwitch:
        case EditorSoundCue::Confirm:
            return EditorFeedbackSoundCategory::Click;
        // The launcher labels this toggle "Errors and warnings".
        case EditorSoundCue::Error:
        case EditorSoundCue::Warning:
            return EditorFeedbackSoundCategory::Error;
        case EditorSoundCue::Button:
        case EditorSoundCue::ButtonDisabled:
        case EditorSoundCue::Tab:
        case EditorSoundCue::ItemSelect:
            return EditorFeedbackSoundCategory::Button;
        case EditorSoundCue::Hover:
            return EditorFeedbackSoundCategory::Hover;
        case EditorSoundCue::ToggleOn:
        case EditorSoundCue::ToggleOff:
            return EditorFeedbackSoundCategory::Toggle;
        case EditorSoundCue::SlideOpen:
        case EditorSoundCue::SlideClose:
            return EditorFeedbackSoundCategory::Slide;
        // A dialog a call site announces is the same event as a modal the widgets report.
        case EditorSoundCue::DialogOpen:
        case EditorSoundCue::PopupOpen:
        case EditorSoundCue::PopupClose:
            return EditorFeedbackSoundCategory::Transition;
        case EditorSoundCue::CompileStart:
        case EditorSoundCue::CompileSuccess:
        case EditorSoundCue::ScriptAssigned:
        case EditorSoundCue::PackageInstalled:
        default:
            return EditorFeedbackSoundCategory::Other;
    }
}

// The same cue again within this window is dropped. Interface sounds layer rather than restart,
// so without it a burst of console errors would stack, a sweep across the menu bar would open a
// menu per frame, and a double click would machine-gun.
static constexpr double kEditorCueRepeatWindowSeconds = 0.06;

// Every themed cue plays through AudioSystem's interface lane, never the preview slot, so none of
// them can cut off the asset or Inspector audio player.
bool Engine::playThemedCue(EditorSoundCue cue, float volume) {
    if (!audio.isReady() || !isEditorFeedbackSoundEnabled(feedbackCategoryForCue(cue))) {
        return false;
    }
    const double now = glfwGetTime();
    double& last = lastCueTime[static_cast<size_t>(cue)];
    if (now - last < kEditorCueRepeatWindowSeconds) {
        return false;
    }
    Modularity::EditorSounds::ResolvedSound sound;
    if (!editorSoundThemes.resolve(cue, sound) || sound.gain <= 0.0f) {
        return false;
    }
    const float level = volume * sound.gain * std::clamp(feedbackSoundVolume, 0.0f, 1.0f);
    if (!audio.playUiSound(sound.path, level)) {
        return false;
    }
    last = now;
    return true;
}

// Call-site cues say more than the generic widget sound for the same click (the Install button
// plays "package installed", not "button"), so one that plays mutes this frame's widget sounds.
bool Engine::playEditorSound(EditorSoundCue cue, float volume) {
    const bool played = playThemedCue(cue, volume);
    if (played) explicitFeedbackCueTime = glfwGetTime();
    return played;
}

void Engine::onWidgetFeedback(ImGuiWidgetFeedback event, void* userData) {
    Engine* engine = static_cast<Engine*>(userData);
    if (!engine || engine->playerMode || event <= ImGuiWidgetFeedback_None || event >= ImGuiWidgetFeedback_COUNT) {
        return;
    }
    engine->pendingWidgetFeedback[static_cast<size_t>(event)] = true;
}

void Engine::flushWidgetFeedbackSounds() {
    std::array<bool, ImGuiWidgetFeedback_COUNT> pending{};
    std::swap(pending, pendingWidgetFeedback);

    // Muted for a moment after a call-site cue, not just its frame: a modal opened after its
    // Begin() reports a frame late, and a double-clicked file opens on the second press but its
    // cell still reports the select on release.
    constexpr double kAfterCallSiteCueSeconds = 0.15;
    if (glfwGetTime() - explicitFeedbackCueTime < kAfterCallSiteCueSeconds) {
        return;
    }

    // One sound per frame, the most telling event first: a button that opens a modal is heard
    // as the modal, a menu command as the command rather than its menu closing.
    struct Mapping {
        ImGuiWidgetFeedback event;
        EditorSoundCue cue;
    };
    static constexpr Mapping kByPriority[] = {
        {ImGuiWidgetFeedback_ModalOpen, EditorSoundCue::PopupOpen},
        {ImGuiWidgetFeedback_ModalClose, EditorSoundCue::PopupClose},
        {ImGuiWidgetFeedback_ToggleOn, EditorSoundCue::ToggleOn},
        {ImGuiWidgetFeedback_ToggleOff, EditorSoundCue::ToggleOff},
        {ImGuiWidgetFeedback_SlideOpen, EditorSoundCue::SlideOpen},
        {ImGuiWidgetFeedback_ButtonDisabled, EditorSoundCue::ButtonDisabled},
        {ImGuiWidgetFeedback_Button, EditorSoundCue::Button},
        {ImGuiWidgetFeedback_Select, EditorSoundCue::ItemSelect},
        {ImGuiWidgetFeedback_Tab, EditorSoundCue::Tab},
        {ImGuiWidgetFeedback_SlideClose, EditorSoundCue::SlideClose},
        {ImGuiWidgetFeedback_Hover, EditorSoundCue::Hover},  // only when nothing else happened
    };

    const double now = glfwGetTime();
    for (const Mapping& mapping : kByPriority) {
        if (!pending[static_cast<size_t>(mapping.event)]) continue;
        // Skip a disabled or silent group and let the next event speak: turning Transitions off
        // should leave the button that opened the dialog audible.
        if (!isEditorFeedbackSoundEnabled(feedbackCategoryForCue(mapping.cue))) continue;
        // A throttled winner ends the frame rather than handing over to a lower event, or a menu
        // bar sweep would trade its dropped slide-open for a slide-close.
        if (now - lastCueTime[static_cast<size_t>(mapping.cue)] < kEditorCueRepeatWindowSeconds) return;
        if (playThemedCue(mapping.cue, 0.95f)) return;
    }
}

void Engine::applyEditorSoundTheme(const std::string& themeId) {
    editorSoundThemes.refresh();
    editorSoundThemes.setActive(themeId);
    for (const std::string& warning : editorSoundThemes.warnings()) {
        addConsoleMessage("Sound theme: " + warning, ConsoleMessageType::Warning);
    }
}

void Engine::selectEditorSoundTheme(std::string themeId) {
    projectManager.preferences.feedbackSoundTheme = themeId;
    applyEditorSoundTheme(themeId);
    projectManager.saveLauncherSettings();
    // Let the user hear what they picked. One-shot, so it does not cut off an asset preview.
    playEditorSound(EditorSoundCue::Confirm, 0.95f);
}

Engine::ScriptAutoCompileScanResult
Engine::runScriptAutoCompileScan(ScriptAutoCompileScanRequest request) {
    ScriptAutoCompileScanResult result;
    result.generation = request.generation;
    result.ranDirectoryScan = request.runDirectoryScan;

    const fs::path& projectRoot = request.projectRoot;

    std::error_code outDirEc;
    std::vector<std::string> outDirPrefixes;
    if (!request.outDir.empty()) {
        fs::path absOut = fs::absolute(request.outDir, outDirEc);
        if (outDirEc) absOut = request.outDir;
        outDirPrefixes.push_back(absOut.lexically_normal().string());
        outDirEc.clear();
        fs::path canonOut = fs::weakly_canonical(request.outDir, outDirEc);
        if (!outDirEc) {
            std::string canonKey = canonOut.lexically_normal().string();
            if (std::find(outDirPrefixes.begin(), outDirPrefixes.end(), canonKey) == outDirPrefixes.end()) {
                outDirPrefixes.push_back(std::move(canonKey));
            }
        }
    }
    auto isGeneratedArtifact = [](const fs::path& path) {
        const std::string name = path.filename().string();
        return name.find(".gen.cpp") != std::string::npos ||
               name.find(".gen.c") != std::string::npos ||
               name.find(".wrap.cpp") != std::string::npos;
    };
    auto isUnderOutDir = [&](const std::string& key) {
        for (const std::string& prefix : outDirPrefixes) {
            if (prefix.empty() || key.size() < prefix.size()) continue;
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            if (key.size() == prefix.size()) return true;
            const char next = key[prefix.size()];
            if (next == '/' || next == '\\') return true;
        }
        return false;
    };

    std::unordered_set<std::string>& sources = result.knownSources;
    auto addSourceTo = [&](std::unordered_set<std::string>& target, const fs::path& path) {
        if (path.empty()) return;
        if (isGeneratedArtifact(path)) return;
        fs::path candidate = path;
        if (candidate.is_relative()) {
            candidate = projectRoot / candidate;
        }
        std::error_code ec;
        fs::path absPath = fs::absolute(candidate, ec);
        if (ec) absPath = candidate;
        std::string key = absPath.lexically_normal().string();
        if (isUnderOutDir(key)) return;
        target.insert(std::move(key));
    };

    for (const fs::path& scriptPath : request.sceneScriptPaths) {
        addSourceTo(sources, scriptPath);
    }

    std::unordered_set<std::string> discovered = std::move(request.discoveredSources);
    std::unordered_set<std::string> scannedRoots;
    auto scanScriptRoot = [&](const fs::path& root) {
        if (root.empty()) return;

        fs::path resolvedRoot = root;
        if (!resolvedRoot.is_absolute()) {
            resolvedRoot = projectRoot / resolvedRoot;
        }

        std::error_code rootEc;
        fs::path normalizedRoot = fs::weakly_canonical(resolvedRoot, rootEc);
        if (rootEc) {
            normalizedRoot = resolvedRoot.lexically_normal();
        }

        std::string rootKey = normalizedRoot.lexically_normal().string();
        if (!scannedRoots.insert(rootKey).second) {
            return;
        }

        std::error_code existsEc;
        if (!fs::exists(normalizedRoot, existsEc) || !fs::is_directory(normalizedRoot, existsEc)) {
            return;
        }

        for (auto it = fs::recursive_directory_iterator(normalizedRoot, existsEc);
             it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_directory()) {
                if (isScriptScanExcludedDir(it->path().filename().string(), it.depth() == 0)) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" ||
                ext == ".moducpp" || ext == ".mko" || ext == ".modumako") {
                addSourceTo(discovered, it->path());
            }
        }
    };

    if (request.runDirectoryScan) {
        discovered.clear();
        scanScriptRoot(projectRoot);
        scanScriptRoot(request.scriptsDir);
    }

    sources.insert(discovered.begin(), discovered.end());
    result.discoveredSources = std::move(discovered);

    result.checkedSourceTime = std::move(request.checkedSourceTime);
    result.binaryCache = std::move(request.binaryCache);
    for (auto it = result.checkedSourceTime.begin(); it != result.checkedSourceTime.end();) {
        if (sources.find(it->first) == sources.end()) {
            it = result.checkedSourceTime.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = result.binaryCache.begin(); it != result.binaryCache.end();) {
        if (sources.find(it->first) == sources.end()) {
            it = result.binaryCache.erase(it);
        } else {
            ++it;
        }
    }

    auto deriveBinaryPath = [&](const fs::path& scriptAbs) -> fs::path {
        std::error_code ec;
        fs::path relToScripts = fs::relative(scriptAbs, request.scriptsDir, ec);
        if (ec) relToScripts.clear();
        bool hasDotDot = false;
        for (const auto& part : relToScripts) { if (part == "..") { hasDotDot = true; break; } }
        if (relToScripts.empty() || relToScripts.is_absolute() || hasDotDot) relToScripts.clear();
        fs::path relativeParent = relToScripts.has_parent_path() ? relToScripts.parent_path() : fs::path();
        std::string baseName = NativeScriptArtifactStem(scriptAbs);
        fs::path binaryPath = request.outDir / relativeParent;
#ifdef _WIN32
        binaryPath /= baseName + ".dll";
#else
        binaryPath /= baseName + ".so";
#endif
        return binaryPath;
    };

    std::unordered_set<std::string> shadowedSources;
    {
        std::unordered_map<std::string, std::vector<std::pair<std::string, fs::file_time_type>>> byArtifact;
        for (const auto& sourceKey : sources) {
            std::error_code ec;
            fs::path sourcePath = sourceKey;
            if (!fs::exists(sourcePath, ec) || ec) continue;
            const auto sourceTime = fs::last_write_time(sourcePath, ec);
            if (ec) continue;
            fs::path scriptAbs = fs::absolute(sourcePath, ec);
            if (ec) scriptAbs = sourcePath;
            const fs::path artifact = deriveBinaryPath(scriptAbs);
            if (artifact.empty()) continue;
            byArtifact[artifact.lexically_normal().string()].push_back({sourceKey, sourceTime});
        }

        for (auto& entry : byArtifact) {
            auto& candidates = entry.second;
            if (candidates.size() < 2) continue;

            std::sort(candidates.begin(), candidates.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });

            std::string message = "Modularity found duplicate scripts ";
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (i > 0) message += ", ";
                message += "\"" + candidates[i].first + "\"";
            }
            message += "; \"" + candidates.front().first +
                       "\" was chosen as it contained the most recent edit.";
            result.duplicateArtifactWarnings.push_back(std::move(message));

            for (size_t i = 1; i < candidates.size(); ++i) {
                shadowedSources.insert(candidates[i].first);
            }
        }
    }

    for (const auto& sourceKey : sources) {
        if (shadowedSources.find(sourceKey) != shadowedSources.end()) continue;

        fs::path sourcePath = sourceKey;
        std::error_code sourceEc;
        if (!fs::exists(sourcePath, sourceEc)) continue;
        auto sourceTime = fs::last_write_time(sourcePath, sourceEc);
        if (sourceEc) continue;

        fs::path binaryPath;
        auto checkedIt = result.checkedSourceTime.find(sourceKey);
        auto cachedBinaryIt = result.binaryCache.find(sourceKey);
        bool canUseCachedBinary = (checkedIt != result.checkedSourceTime.end() &&
                                   checkedIt->second == sourceTime &&
                                   cachedBinaryIt != result.binaryCache.end());
        if (canUseCachedBinary) {
            binaryPath = cachedBinaryIt->second;
        } else {
            fs::path scriptAbs = fs::absolute(sourcePath, sourceEc);
            if (sourceEc) scriptAbs = sourcePath;
            binaryPath = deriveBinaryPath(scriptAbs);
            if (binaryPath.empty()) continue;
            result.binaryCache[sourceKey] = binaryPath;
            result.checkedSourceTime[sourceKey] = sourceTime;
        }

        std::error_code binEc;
        bool binaryExists = !binaryPath.empty() && fs::exists(binaryPath, binEc);
        fs::file_time_type binaryTime{};
        if (binaryExists && !binEc) {
            binaryTime = fs::last_write_time(binaryPath, binEc);
        }
        const bool binaryUsable = binaryExists && !binEc;

        if (binaryUsable && request.toolchainStamp.valid() &&
            request.historyForcedKeys.find(sourceKey) == request.historyForcedKeys.end()) {
            const auto historyIt = request.historyEntries.find(sourceKey);
            if (historyIt == request.historyEntries.end()) {
                if (request.historyMigrating) {
                    ScriptHistoryEntry adopted;
                    adopted.stamp = request.toolchainStamp;
                    adopted.binaryPath = binaryPath;
                    adopted.binaryWriteTime = ScriptHistory::toTimeRep(binaryTime);
                    result.adoptedHistoryEntries.emplace_back(sourceKey, std::move(adopted));
                } else {
                    ++result.historyStaleCount;
                    result.outdatedSources.push_back({std::move(sourcePath), sourceTime, true});
                    continue;
                }
            } else if (!scriptToolchainStampsCompatible(historyIt->second.stamp,
                                                        request.toolchainStamp) ||
                       historyIt->second.binaryWriteTime != ScriptHistory::toTimeRep(binaryTime)) {
                ++result.historyStaleCount;
                result.outdatedSources.push_back({std::move(sourcePath), sourceTime, true});
                continue;
            }
        }

        if (binaryUsable && sourceTime <= binaryTime) continue;

        auto it = request.lastAutoCompileTime.find(sourceKey);
        if (it != request.lastAutoCompileTime.end() && sourceTime <= it->second) continue;

        result.outdatedSources.push_back({std::move(sourcePath), sourceTime, false});
    }

    if (request.scanManaged) {
        result.managedScanned = true;
        result.managedHasSource = request.managedHasSource;
        result.managedNewestSource = request.managedNewestSource;

        if (request.runManagedDirectoryScan) {
            result.managedHasSource = false;
            result.managedNewestSource = fs::file_time_type{};

            std::error_code scanEc;
            const fs::path managedDir = request.managedProject.parent_path();
            if (fs::exists(managedDir, scanEc)) {
                for (auto it = fs::recursive_directory_iterator(managedDir, scanEc);
                     it != fs::recursive_directory_iterator(); ++it) {
                    if (it->is_directory()) {
                        if (isManagedGeneratedPath(it->path())) {
                            it.disable_recursion_pending();
                        }
                        continue;
                    }
                    if (it->path().extension() != ".cs") continue;
                    if (isManagedGeneratedPath(it->path())) continue;
                    auto sourceTime = fs::last_write_time(it->path(), scanEc);
                    if (scanEc) continue;
                    if (!result.managedHasSource || sourceTime > result.managedNewestSource) {
                        result.managedNewestSource = sourceTime;
                        result.managedHasSource = true;
                    }
                }
            }
        }

        std::error_code managedEc;
        bool needsManaged = false;
        if (!fs::exists(request.managedOutput, managedEc)) {
            needsManaged = true;
        } else if (result.managedHasSource && !managedEc) {
            auto binaryTime = fs::last_write_time(request.managedOutput, managedEc);
            if (!managedEc && result.managedNewestSource > binaryTime) {
                needsManaged = true;
            }
        }

        if (needsManaged && request.managedHasCompiled &&
            result.managedNewestSource <= request.managedCompiledSource) {
            needsManaged = false;
        }

        result.managedNeedsCompile = needsManaged;
    }

    return result;
}

void Engine::applyScriptAutoCompileScanResult(ScriptAutoCompileScanResult& result) {
    if (result.generation != scriptAutoCompileScanGeneration) return;

    scriptAutoCompileDiscoveredSources = std::move(result.discoveredSources);
    scriptAutoCompileCheckedSourceTime = std::move(result.checkedSourceTime);
    scriptAutoCompileBinaryCache = std::move(result.binaryCache);

    for (auto it = scriptLastAutoCompileTime.begin(); it != scriptLastAutoCompileTime.end();) {
        if (result.knownSources.find(it->first) == result.knownSources.end()) {
            it = scriptLastAutoCompileTime.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& adopted : result.adoptedHistoryEntries) {
        if (scriptHistory.find(adopted.first) == nullptr) {
            scriptHistory.record(adopted.first, adopted.second);
        }
    }
    if (!result.adoptedHistoryEntries.empty() || result.ranDirectoryScan) {
        if (result.ranDirectoryScan) {
            scriptHistory.retainOnly(result.knownSources);
        }
        flushScriptHistory();
    }

    for (const std::string& warning : result.duplicateArtifactWarnings) {
        addConsoleMessage(warning, ConsoleMessageType::Warning);
    }

    if (result.historyStaleCount > 0) {
        addConsoleMessage(
            "Script ABI changed since these were compiled; rebuilding " +
                std::to_string(result.historyStaleCount) +
                (result.historyStaleCount == 1 ? " script" : " scripts"),
            ConsoleMessageType::Info);
    }

    for (const auto& entry : result.outdatedSources) {
        if (entry.forcedByHistory) {
            scriptHistoryForcedRecompile.insert(ScriptHistory::makeKey(entry.path));
        } else {
            std::error_code ec;
            fs::path scriptAbs = fs::absolute(entry.path, ec);
            if (ec) scriptAbs = entry.path;
            auto queuedIt = scriptLastAutoCompileTime.find(scriptAbs.lexically_normal().string());
            if (queuedIt != scriptLastAutoCompileTime.end() && entry.sourceTime <= queuedIt->second) {
                continue;
            }
        }
        queueAutoCompile(entry.path, entry.sourceTime);
    }

    if (!result.managedScanned) return;

    managedAutoCompileHasSource = result.managedHasSource;
    managedAutoCompileNewestSource = result.managedNewestSource;
    if (result.managedNeedsCompile) {
        managedAutoCompileCompiledSource = managedAutoCompileNewestSource;
        managedAutoCompileHasCompiled = true;
        nextCompileFromAuto = true;
        if (!compileInProgress) {
            compileManagedScripts();
        } else {
            managedAutoCompileQueued = true;
        }
        nextCompileFromAuto = false;
    }
}

void Engine::updateAutoCompileScripts() {
    if (scriptAutoCompileScanInFlight && scriptAutoCompileScanFuture.valid() &&
        scriptAutoCompileScanFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ScriptAutoCompileScanResult result = scriptAutoCompileScanFuture.get();
        scriptAutoCompileScanInFlight = false;
        if (projectManager.currentProject.isLoaded && !showLauncher &&
            !isPlaying && !specMode && !testMode) {
            applyScriptAutoCompileScanResult(result);
        }
    }

    if (!scriptAutoCompileEnabled) return;
    if (!projectManager.currentProject.isLoaded) return;
    if (showLauncher) return;
    if (isPlaying || specMode || testMode) return;
    if (scriptAutoCompileScanInFlight) return;

    double now = glfwGetTime();
    if (now - scriptAutoCompileLastCheck < scriptAutoCompileInterval) return;
    scriptAutoCompileLastCheck = now;

    fs::path configPath = resolveScriptsConfigPath(projectManager.currentProject);

    std::error_code configEc;
    fs::file_time_type configTime{};
    bool configTimeKnown = false;
    if (fs::exists(configPath, configEc) && !configEc) {
        configTime = fs::last_write_time(configPath, configEc);
        configTimeKnown = !configEc;
    }
    if (!scriptAutoCompileConfigValid || configPath != scriptAutoCompileConfigPath ||
        !configTimeKnown || configTime != scriptAutoCompileConfigTime) {
        ScriptBuildConfig config;
        std::string error;
        if (!scriptCompiler.loadConfig(configPath, config, error)) {
            scriptAutoCompileConfigValid = false;
            return;
        }
        packageManager.applyToBuildConfig(config);
        scriptAutoCompileConfig = std::move(config);
        scriptAutoCompileConfigPath = configPath;
        scriptAutoCompileConfigTime = configTime;
        scriptAutoCompileConfigValid = true;
        scriptHistoryPath.clear();
    }

    ensureScriptHistoryLoaded();

    ScriptAutoCompileScanRequest request;
    request.projectRoot = projectManager.currentProject.projectPath;
    request.scriptsDir = scriptAutoCompileConfig.scriptsDir;
    request.outDir = scriptAutoCompileConfig.outDir;
    request.generation = scriptAutoCompileScanGeneration;

    bool hasManagedScripts = false;
    for (const auto& obj : sceneObjects) {
        for (const auto& sc : obj.scripts) {
            if (sc.language == ScriptLanguage::CSharp) {
                hasManagedScripts = true;
                continue;
            }
            if (sc.path.empty()) continue;
            request.sceneScriptPaths.push_back(sc.path);
        }
    }

    request.runDirectoryScan =
        (now - scriptAutoCompileLastDirectoryScan >= scriptAutoCompileDirectoryScanInterval);
    if (request.runDirectoryScan) {
        scriptAutoCompileLastDirectoryScan = now;
    }
    if (request.runDirectoryScan || !scriptToolchainStamp.valid()) {
        const ScriptToolchainStamp refreshed =
            currentScriptToolchainStamp(scriptAutoCompileConfig.includeDirs);
        if (!scriptToolchainStampsCompatible(scriptToolchainStamp, refreshed)) {
            scriptToolchainStamp = refreshed;
            scriptHistory.setStamp(refreshed);
        }
    }
    request.toolchainStamp = scriptToolchainStamp;
    request.historyMigrating = scriptHistory.isMigrating();
    request.historyForcedKeys = scriptHistoryForcedRecompile;
    request.historyEntries = scriptHistory.entries();

    request.discoveredSources = std::move(scriptAutoCompileDiscoveredSources);
    scriptAutoCompileDiscoveredSources.clear();
    request.checkedSourceTime = std::move(scriptAutoCompileCheckedSourceTime);
    scriptAutoCompileCheckedSourceTime.clear();
    request.binaryCache = std::move(scriptAutoCompileBinaryCache);
    scriptAutoCompileBinaryCache.clear();
    request.lastAutoCompileTime = scriptLastAutoCompileTime;

    if (hasManagedScripts) {
        std::error_code managedEc;
        fs::path projectManagedProject =
            projectManager.currentProject.projectPath / "Scripts" / "Managed" / "ModuCPP.csproj";
        fs::path managedProject = fs::exists(projectManagedProject, managedEc)
            ? projectManagedProject
            : getManagedProjectPath();
        if (fs::exists(managedProject, managedEc)) {
            const fs::path managedDir = managedProject.parent_path();
            request.scanManaged = true;
            request.managedProject = managedProject;
            request.managedOutput = managedOutputPathFromProject(managedProject);
            request.runManagedDirectoryScan =
                (managedDir != managedAutoCompileCachedProjectDir) ||
                (now - managedAutoCompileLastScan >= managedAutoCompileScanInterval);
            if (request.runManagedDirectoryScan) {
                managedAutoCompileCachedProjectDir = managedDir;
                managedAutoCompileLastScan = now;
            }
            request.managedHasSource = managedAutoCompileHasSource;
            request.managedNewestSource = managedAutoCompileNewestSource;
            request.managedCompiledSource = managedAutoCompileCompiledSource;
            request.managedHasCompiled = managedAutoCompileHasCompiled;
        }
    }

    scriptAutoCompileScanFuture =
        std::async(std::launch::async, &Engine::runScriptAutoCompileScan, std::move(request));
    scriptAutoCompileScanInFlight = true;
}

void Engine::processAutoCompileQueue() {
    if (isPlaying || specMode || testMode) return;
    if (compileInProgress) return;
    if (autoCompileQueue.empty()) return;

    std::vector<fs::path> batch;
    batch.reserve(autoCompileQueue.size());

    while (!autoCompileQueue.empty()) {
        fs::path next = autoCompileQueue.front();
        autoCompileQueue.pop_front();

        std::error_code ec;
        fs::path absPath = fs::absolute(next, ec);
        if (ec) absPath = next;
        autoCompileQueued.erase(absPath.lexically_normal().string());

        batch.push_back(std::move(next));
    }

    if (!batch.empty()) {
        nextCompileFromAuto = true;
        queueScriptCompileBatch(batch);
        nextCompileFromAuto = false;
    }
}

void Engine::updatePlayerController(float delta) {
    if (!isPlaying) {
        playerViewMotionCameraId = -1;
        playerViewMotionOffset = glm::vec3(0.0f);
        return;
    }

    SceneObject* player = nullptr;
    for (auto& obj : sceneObjects) {
        if (IsObjectEnabledInHierarchy(obj) && obj.hasPlayerController && obj.playerController.enabled) {
            player = &obj;
            activePlayerId = obj.id;
            break;
        }
    }
    if (!player) {
        activePlayerId = -1;
        playerControllerGroundProbeDebug = {};
        playerControllerRuntimeStates.clear();
        playerViewMotionCameraId = -1;
        playerViewMotionOffset = glm::vec3(0.0f);
        return;
    }

    for (auto it = playerControllerRuntimeStates.begin();
         it != playerControllerRuntimeStates.end();) {
        if (it->first != player->id) {
            it = playerControllerRuntimeStates.erase(it);
        } else {
            ++it;
        }
    }
    PlayerControllerRuntimeState& runtime = playerControllerRuntimeStates[player->id];
    auto moveTowardsVec2 = [](const glm::vec2& current, const glm::vec2& target, float maxDelta) {
        if (maxDelta <= 0.0f) return current;
        glm::vec2 deltaVec = target - current;
        float len = glm::length(deltaVec);
        if (len <= maxDelta || len <= 1e-5f) {
            return target;
        }
        return current + (deltaVec / len) * maxDelta;
    };
    auto sanitizePlanar = [](const glm::vec3& value) {
        glm::vec3 out(value.x, 0.0f, value.z);
        if (!std::isfinite(out.x) || !std::isfinite(out.z)) {
            return glm::vec3(0.0f);
        }
        return out;
    };

    auto& pc = player->playerController;
    // Maintain capsule sizing and collider defaults
    if (pc.pitch == 0.0f && pc.yaw == 0.0f && (glm::length(player->rotation) > 0.01f)) {
        const glm::vec3 authoredForward =
            EulerXYZDegreesToQuat(player->rotation) * glm::vec3(0.0f, 0.0f, -1.0f);
        if (glm::length(authoredForward) > 1e-4f) {
            const glm::vec3 f = glm::normalize(authoredForward);
            pc.pitch = glm::degrees(std::asin(std::clamp(f.y, -1.0f, 1.0f)));
            pc.yaw = glm::degrees(std::atan2(-f.x, -f.z));
        }
    }
    glm::vec3 capsuleSize(pc.radius * 2.0f, pc.height, pc.radius * 2.0f);
    player->hasCollider = true;
    player->collider.type = ColliderType::Capsule;
    player->collider.convex = true;
    player->collider.boxSize = capsuleSize;
    player->scale = capsuleSize;
    player->hasRigidbody = true;
    player->rigidbody.enabled = true;
    player->rigidbody.useGravity = true;
    player->rigidbody.isKinematic = false;
    player->rigidbody.lockRotationX = true;
    player->rigidbody.lockRotationY = false;
    player->rigidbody.lockRotationZ = true;

    // Mouse look when game viewport is focused
    const PlayerControlFeelSettings& feel = player->playerControlFeel;
    glm::vec2 lookAngleDelta(0.0f);
    if (gameViewportFocused || gameViewCursorLocked) {
        ImGuiIO& io = ImGui::GetIO();
        const float yawBeforeLook = pc.yaw;
        const float pitchBeforeLook = pc.pitch;

        runtime.pendingLook += glm::vec2(-io.MouseDelta.y * 50.0f * pc.lookSensitivity * delta,
                                         -io.MouseDelta.x * 50.0f * pc.lookSensitivity * delta);
        glm::vec2 appliedLook = runtime.pendingLook;
        const float lookSmoothing = std::clamp(feel.lookSmoothing, 0.0f, 1.0f);
        if (lookSmoothing > 0.0f && delta > 0.0f) {
            const float tau = lookSmoothing * 0.2f;
            appliedLook = runtime.pendingLook * (1.0f - std::exp(-delta / tau));
        }
        runtime.pendingLook -= appliedLook;

        pc.pitch += appliedLook.x;
        pc.yaw += appliedLook.y;
        const float clampedPitch = std::clamp(pc.pitch, -89.0f, 89.0f);
        if (clampedPitch != pc.pitch) {
            runtime.pendingLook.x = 0.0f;
            pc.pitch = clampedPitch;
        }
        lookAngleDelta = glm::vec2(pc.pitch - pitchBeforeLook, pc.yaw - yawBeforeLook);
    } else {
        runtime.pendingLook = glm::vec2(0.0f);
    }

    // Movement input aligned to camera facing (-Z forward convention)
    auto key = [&](int k) { return glfwGetKey(editorWindow, k) == GLFW_PRESS; };
    glm::quat q = glm::quat(glm::radians(glm::vec3(pc.pitch, pc.yaw, 0.0f)));
    glm::vec3 forward = glm::normalize(q * glm::vec3(0.0f, 0.0f, -1.0f));
    glm::vec3 right = glm::normalize(q * glm::vec3(1.0f, 0.0f, 0.0f));
    glm::vec3 planarForward = glm::normalize(glm::vec3(forward.x, 0.0f, forward.z));
    glm::vec3 planarRight = glm::normalize(glm::vec3(right.x, 0.0f, right.z));
    if (!std::isfinite(planarForward.x) || glm::length(planarForward) < 1e-3f) {
        planarForward = glm::vec3(0, 0, -1);
    }
    if (!std::isfinite(planarRight.x) || glm::length(planarRight) < 1e-3f) {
        planarRight = glm::vec3(1, 0, 0);
    }

    glm::vec3 move(0.0f);
    if (key(GLFW_KEY_W)) move += planarForward;
    if (key(GLFW_KEY_S)) move -= planarForward;
    if (key(GLFW_KEY_D)) move += planarRight;
    if (key(GLFW_KEY_A)) move -= planarRight;
    if (glm::length(move) > 0.001f) move = glm::normalize(move);

    glm::vec2 localInput(glm::dot(move, planarRight), glm::dot(move, planarForward));
    if (glm::length(localInput) > 1.0f) {
        localInput = glm::normalize(localInput);
    }
    bool sprinting = key(GLFW_KEY_LEFT_SHIFT) || key(GLFW_KEY_RIGHT_SHIFT);
    float targetSpeed = sprinting ? std::max(pc.moveSpeed, pc.runSpeed) : pc.moveSpeed;
    glm::vec2 targetLocalVelocity = localInput * targetSpeed;
    glm::vec3 velocity(0.0f);

    // Simple gravity and jump
    float capsuleHalf = std::max(0.1f, pc.height * 0.5f);
    float groundProbeLead = std::clamp(capsuleHalf * 0.08f, 0.08f, 0.24f);
    float groundProbeDepth = std::max(0.45f, capsuleHalf * 0.35f);
    float groundSnap = std::clamp(capsuleHalf * 0.12f, 0.2f, 0.45f);
    glm::vec3 physVel;
    bool havePhysVel = physics->getLinearVelocity(player->id, physVel);
    if (havePhysVel) pc.verticalVelocity = physVel.y;

    // Ground check via PhysX scene query so mesh colliders work, not just the plane
    glm::vec3 hitPos;
    glm::vec3 hitNormal;
    glm::vec3 hitActorVelocity(0.0f);
    int hitActorId = -1;
    float hitStaticFriction = 0.9f;
    float hitDynamicFriction = 0.9f;
    float hitDist = 0.0f;
    glm::vec3 rayStart = player->position + glm::vec3(0.0f, -capsuleHalf + groundProbeLead, 0.0f);
    float probeDist = groundProbeLead + groundProbeDepth;
    glm::vec3 rayEnd = rayStart + glm::vec3(0.0f, -probeDist, 0.0f);
    bool hitGround = physics->raycastClosest(rayStart, glm::vec3(0.0f, -1.0f, 0.0f), probeDist,
                                            player->id, &hitPos, &hitNormal, &hitDist,
                                            &hitActorId, &hitActorVelocity,
                                            &hitStaticFriction, &hitDynamicFriction);
    bool grounded = hitGround && hitNormal.y > 0.25f && hitDist <= groundProbeLead + groundSnap && pc.verticalVelocity <= 0.35f;

    playerControllerGroundProbeDebug.playerId = player->id;
    playerControllerGroundProbeDebug.rayStart = rayStart;
    playerControllerGroundProbeDebug.rayEnd = rayEnd;
    playerControllerGroundProbeDebug.hitPos = hitGround ? hitPos : rayEnd;
    playerControllerGroundProbeDebug.hasHit = hitGround;

    (void)hitActorId;
    (void)hitStaticFriction;
    const float dynamicFriction = std::clamp(hitDynamicFriction, 0.0f, 2.0f);
    const float minSurfaceControl = std::clamp(pc.minSurfaceControl, 0.0f, 1.0f);
    const float grip = grounded ? std::clamp(dynamicFriction, minSurfaceControl, 1.0f) : 1.0f;
    const float groundAccel = std::max(0.0f, pc.groundAcceleration);
    const float airAccel = std::max(0.0f, pc.airAcceleration);
    const float braking = std::max(0.0f, pc.braking);
    const float slideGravity = std::max(0.0f, pc.slideGravity);
    const float platformCarry = std::clamp(pc.platformCarry, 0.0f, 3.0f);

    float accelRate = grounded ? groundAccel * grip : airAccel;
    runtime.localVelocity = (accelRate > 0.0f)
        ? moveTowardsVec2(runtime.localVelocity, targetLocalVelocity, accelRate * delta)
        : targetLocalVelocity;
    if (glm::dot(localInput, localInput) < 1e-4f && braking > 0.0f) {
        float brakeScale = grounded ? (0.5f + 0.5f * grip) : 0.15f;
        float damp = std::max(0.0f, 1.0f - braking * brakeScale * delta);
        runtime.localVelocity *= damp;
    }
    float localSpeed = glm::length(runtime.localVelocity);
    if (localSpeed > targetSpeed && targetSpeed > 0.0f) {
        runtime.localVelocity *= (targetSpeed / localSpeed);
    }

    glm::vec3 platformVelocity = sanitizePlanar(hitActorVelocity);
    if (grounded && hitGround) {
        if (runtime.hasGroundSample && delta > 1e-5f) {
            glm::vec3 pointVelocity = sanitizePlanar((hitPos - runtime.lastGroundHitPos) / delta);
            if (glm::dot(pointVelocity, pointVelocity) < (120.0f * 120.0f)) {
                if (glm::dot(platformVelocity, platformVelocity) < 1e-4f) {
                    platformVelocity = pointVelocity;
                } else {
                    platformVelocity = glm::mix(platformVelocity, pointVelocity, 0.35f);
                }
            }
        }
        runtime.lastGroundHitPos = hitPos;
        runtime.hasGroundSample = true;
    } else {
        runtime.hasGroundSample = false;
    }

    if (grounded && hitGround) {
        glm::vec3 n = glm::normalize(hitNormal);
        if (std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z)) {
            glm::vec3 gravityDir(0.0f, -1.0f, 0.0f);
            glm::vec3 downSlope = gravityDir - n * glm::dot(gravityDir, n);
            float downLen = glm::length(downSlope);
            if (downLen > 1e-4f) {
                downSlope /= downLen;
                float slopeFactor = std::clamp((1.0f - n.y) * 3.0f, 0.0f, 1.5f);
                float slip = std::clamp(1.0f - dynamicFriction * 0.85f, 0.0f, 1.0f);
                float slideAccel = slideGravity * slopeFactor * (0.35f + slip);
                runtime.slideVelocity += downSlope * slideAccel * delta;
            }
        }
        float slideDamp = std::clamp((dynamicFriction + 0.15f) * 6.0f, 0.5f, 12.0f);
        runtime.slideVelocity *= std::max(0.0f, 1.0f - slideDamp * delta);
    } else {
        runtime.slideVelocity *= std::max(0.0f, 1.0f - 4.0f * delta);
    }
    runtime.slideVelocity = sanitizePlanar(runtime.slideVelocity);

    const float landingImpactSpeed = std::max(0.0f, -pc.verticalVelocity);
    bool jumpLaunchedThisFrame = false;

    if (grounded) {
        pc.verticalVelocity = 0.0f;
        if (!havePhysVel && hitGround) {
            player->position.y = std::max(player->position.y, hitPos.y + capsuleHalf);
        }

        const float kGroundContactEpsilon = 0.03f;
        const float groundGap = hitDist - groundProbeLead;
        if (hitGround && groundGap > kGroundContactEpsilon) {
            const float snapDelta = std::max(delta, 1.0f / 60.0f);
            pc.verticalVelocity = -(groundGap / snapDelta);
        }

        const bool jumpHeld = key(GLFW_KEY_SPACE);
        const auto launchJump = [&](float strength) {
            pc.verticalVelocity = strength;
            runtime.hasGroundSample = false;
            runtime.jumpCharging = false;
            runtime.jumpCharge = 0.0f;
            jumpLaunchedThisFrame = true;
        };

        if (!feel.chargedJump) {
            if (jumpHeld) launchJump(pc.jumpStrength);
        } else {
            const float chargeTime = std::max(0.01f, feel.jumpChargeTime);
            if (jumpHeld) {
                runtime.jumpCharging = true;
                runtime.jumpCharge = std::min(runtime.jumpCharge + std::max(0.0f, delta), chargeTime);
                if (runtime.jumpCharge >= chargeTime) {
                    launchJump(pc.jumpStrength);
                }
            } else if (runtime.jumpCharging) {
                const float chargeRatio = std::clamp(runtime.jumpCharge / chargeTime, 0.0f, 1.0f);
                const float scale = glm::mix(std::clamp(feel.jumpChargeMinScale, 0.0f, 1.0f), 1.0f, chargeRatio);
                launchJump(pc.jumpStrength * scale);
            }
        }
    } else {
        pc.verticalVelocity += -9.81f * delta;
        runtime.jumpCharging = false;
        runtime.jumpCharge = 0.0f;
    }

    platformVelocity = grounded ? platformVelocity * platformCarry : glm::vec3(0.0f);
    glm::vec3 planarVelocity =
        planarRight * runtime.localVelocity.x +
        planarForward * runtime.localVelocity.y +
        platformVelocity +
        runtime.slideVelocity;

    velocity.x = planarVelocity.x;
    velocity.z = planarVelocity.z;
    velocity.y = pc.verticalVelocity;
    velocity.y = std::clamp(velocity.y, -30.0f, 30.0f);

    // Apply yaw to physics actor and keep collider aligned
    physics->setActorYaw(player->id, pc.yaw);
    glm::vec3 viewAngleOffsetDeg(0.0f);
    const float jumpChargeRatio = runtime.jumpCharging
        ? std::clamp(runtime.jumpCharge / std::max(0.01f, feel.jumpChargeTime), 0.0f, 1.0f)
        : 0.0f;
    const float planarSpeed = glm::length(runtime.localVelocity);

    const bool landedThisFrame = grounded && !runtime.wasGrounded;
    runtime.wasGrounded = grounded;
    const int footfalls = updatePlayerGait(*player, runtime, planarSpeed, grounded, delta);
    updatePlayerMovementAudio(*player, runtime, footfalls, jumpLaunchedThisFrame,
                              landedThisFrame, landingImpactSpeed);

    updatePlayerViewMotion(*player, runtime, lookAngleDelta,
                           landingImpactSpeed, landedThisFrame, jumpChargeRatio,
                           jumpLaunchedThisFrame, delta, viewAngleOffsetDeg);

    const glm::quat lookRotation =
        glm::angleAxis(glm::radians(pc.yaw + viewAngleOffsetDeg.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(glm::radians(pc.pitch + viewAngleOffsetDeg.x), glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::angleAxis(glm::radians(viewAngleOffsetDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    player->rotation = ChooseContinuousEulerDegrees(QuatToEulerXYZDegrees(lookRotation),
                                                   player->rotation);

    if (!physics->setLinearVelocity(player->id, velocity)) {
        player->position += velocity * delta;
    }
    syncLocalTransform(*player);
}

namespace {
float PlayerGaitWalkWeight(float gaitSpeed, float walkSpeed) {
    return std::clamp(gaitSpeed / (std::max(0.001f, walkSpeed) * 0.45f), 0.0f, 1.0f);
}
float PlayerGaitRunWeight(float gaitSpeed, float walkSpeed, float runSpeed) {
    return std::clamp((gaitSpeed - walkSpeed) / std::max(0.001f, runSpeed - walkSpeed), 0.0f, 1.0f);
}
} // namespace

int Engine::updatePlayerGait(SceneObject& player,
                             PlayerControllerRuntimeState& runtime,
                             float planarSpeed,
                             bool grounded,
                             float delta) {
    const PlayerViewMotionSettings& vm = player.playerViewMotion;
    const PlayerControllerComponent& pc = player.playerController;
    const float dt = std::max(0.0f, delta);
    const float kTau = 6.28318530718f;

    const float walkSpeed = std::max(0.001f, pc.moveSpeed);
    const float runSpeed = std::max(walkSpeed + 0.001f, pc.runSpeed);

    if (dt > 0.0f) {
        const float rate = std::max(0.01f, vm.gaitBlend);
        runtime.gaitSpeed += ((grounded ? planarSpeed : 0.0f) - runtime.gaitSpeed) *
                             (1.0f - std::exp(-rate * dt));
    }

    const float walkWeight = PlayerGaitWalkWeight(runtime.gaitSpeed, walkSpeed);
    int footfalls = 0;

    if (walkWeight < 0.01f) {
        runtime.bobPhase = 0.0f;
        runtime.footPhase = 0.25f;
    } else if (dt > 0.0f) {
        const float strideHz = std::max(0.0f, vm.bobFrequency) *
                               std::min(runtime.gaitSpeed / walkSpeed, runSpeed / walkSpeed);
        runtime.bobPhase = std::fmod(runtime.bobPhase + strideHz * kTau * dt, kTau);

        runtime.footPhase += strideHz * 2.0f * dt;
        while (runtime.footPhase >= 1.0f) {
            runtime.footPhase -= 1.0f;
            ++footfalls;
            if (footfalls >= 2) {
                runtime.footPhase = std::fmod(runtime.footPhase, 1.0f);
                break;
            }
        }
    }

    return grounded ? footfalls : 0;
}

void Engine::updatePlayerMovementAudio(SceneObject& player,
                                       PlayerControllerRuntimeState& runtime,
                                       int footfalls,
                                       bool jumped,
                                       bool landed,
                                       float landingImpactSpeed) {
    const PlayerMovementAudioSettings& ma = player.playerMovementAudio;
    if (!ma.enabled) return;

    const PlayerControllerComponent& pc = player.playerController;
    const float walkSpeed = std::max(0.001f, pc.moveSpeed);
    const float runSpeed = std::max(walkSpeed + 0.001f, pc.runSpeed);
    const float runWeight = PlayerGaitRunWeight(runtime.gaitSpeed, walkSpeed, runSpeed);

    const float variance = std::clamp(ma.pitchVariance, 0.0f, 0.9f);
    auto playClip = [&](const std::string& clip, float volumeScale) {
        if (clip.empty()) return;
        float pitch = 1.0f;
        if (variance > 0.0f) {
            std::uniform_real_distribution<float> pitchDist(1.0f - variance, 1.0f + variance);
            pitch = pitchDist(movementAudioRng);
        }
        audio.playOneShot(clip, std::max(0.0f, ma.volume) * std::max(0.0f, volumeScale), pitch);
    };

    for (int step = 0; step < footfalls && !ma.footstepClips.empty(); ++step) {
        int index = 0;
        if (ma.footstepClips.size() > 1) {
            std::uniform_int_distribution<size_t> clipDist(0, ma.footstepClips.size() - 1);
            index = static_cast<int>(clipDist(movementAudioRng));
            if (index == runtime.lastFootstepClip) {
                index = (index + 1) % static_cast<int>(ma.footstepClips.size());
            }
        }
        runtime.lastFootstepClip = index;
        playClip(ma.footstepClips[static_cast<size_t>(index)],
                 glm::mix(1.0f, std::max(0.0f, ma.runVolumeScale), runWeight));
    }

    if (jumped) {
        playClip(ma.jumpClip, 1.0f);
    }
    if (landed) {
        const float impact = std::clamp(landingImpactSpeed / 12.0f, 0.0f, 1.0f);
        playClip(ma.landClip, std::max(0.0f, ma.landVolumeScale) * glm::mix(0.45f, 1.0f, impact));
    }
}

void Engine::releasePlayerViewMotionChildren(PlayerControllerRuntimeState& runtime) {
    for (const auto& [childId, base] : runtime.attachedBase) {
        if (SceneObject* child = findObjectById(childId)) {
            child->localRotation = base.localRotation;
            if (base.movedPosition) child->localPosition = base.localPosition;
            child->localInitialized = true;
        }
    }
    runtime.attachedBase.clear();
    runtime.heldAirLift = 0.0f;
    runtime.heldDip = 0.0f;
    runtime.heldDipVelocity = 0.0f;
    runtime.heldTilt = 0.0f;
    runtime.heldTiltVelocity = 0.0f;
}

void Engine::updatePlayerViewMotion(SceneObject& player,
                                    PlayerControllerRuntimeState& runtime,
                                    const glm::vec2& lookAngleDelta,
                                    float landingImpactSpeed,
                                    bool landed,
                                    float jumpChargeRatio,
                                    bool jumpLaunched,
                                    float delta,
                                    glm::vec3& outAngleOffsetDeg) {
    const PlayerViewMotionSettings& vm = player.playerViewMotion;
    const PlayerControllerComponent& pc = player.playerController;

    outAngleOffsetDeg = glm::vec3(0.0f);
    playerViewMotionCameraId = -1;
    playerViewMotionOffset = glm::vec3(0.0f);

    if (!vm.enabled) {
        releasePlayerViewMotionChildren(runtime);
        return;
    }

    const float dt = std::max(0.0f, delta);
    const float kTau = 6.28318530718f;

    const auto smoothTo = [](float current, float target, float rate, float step) {
        if (rate <= 0.0f || step <= 0.0f) return current;
        return current + (target - current) * (1.0f - std::exp(-rate * step));
    };
    const auto springStep = [](float& offset, float& velocity,
                               float stiffness, float damping, float step) {
        if (step <= 0.0f) return;
        velocity += -offset * stiffness * step;
        velocity *= std::exp(-std::max(0.0f, damping) * step);
        offset += velocity * step;
    };

    const float walkSpeed = std::max(0.001f, pc.moveSpeed);
    const float runSpeed = std::max(walkSpeed + 0.001f, pc.runSpeed);

    const float walkWeight = PlayerGaitWalkWeight(runtime.gaitSpeed, walkSpeed);
    const float runWeight = PlayerGaitRunWeight(runtime.gaitSpeed, walkSpeed, runSpeed);
    const float bobAmplitude = glm::mix(1.0f, std::max(0.0f, vm.runMultiplier), runWeight);
    const float bobWeight = walkWeight * bobAmplitude;
    const float runLeanDeg = -vm.runLean * runWeight;
    const float bobVertical = std::sin(runtime.bobPhase * 2.0f) * vm.bobVertical * bobWeight;
    const float bobLateral = std::sin(runtime.bobPhase) * vm.bobHorizontal * bobWeight;
    const float bobRollDeg = std::sin(runtime.bobPhase) * vm.bobRoll * bobWeight;

    if (dt > 0.0f) runtime.idlePhase += dt;
    runtime.idleWeight = smoothTo(runtime.idleWeight, 1.0f - walkWeight,
                                  std::max(0.01f, vm.idleBlend), dt);
    const float idleWeight = runtime.idleWeight;
    const float breath =
        std::sin(runtime.idlePhase * kTau * vm.idleFrequency) * 0.62f +
        std::sin(runtime.idlePhase * kTau * vm.idleFrequency * 0.47f + 1.3f) * 0.38f;
    const float idlePitchDeg = breath * vm.idleAmount * idleWeight;
    const float idleVertical = breath * vm.idleAmount * 0.01f * idleWeight;

    runtime.lookSwayVelocity += glm::vec2(-lookAngleDelta.x, -lookAngleDelta.y) * vm.lookSway;
    springStep(runtime.lookSwayOffset.x, runtime.lookSwayVelocity.x,
               vm.lookSwayStiffness, vm.lookSwayDamping, dt);
    springStep(runtime.lookSwayOffset.y, runtime.lookSwayVelocity.y,
               vm.lookSwayStiffness, vm.lookSwayDamping, dt);
    const float lookSwayMax = std::max(0.0f, vm.lookSwayMax);
    runtime.lookSwayOffset.x = std::clamp(runtime.lookSwayOffset.x, -lookSwayMax, lookSwayMax);
    runtime.lookSwayOffset.y = std::clamp(runtime.lookSwayOffset.y, -lookSwayMax, lookSwayMax);

    const float strafeRatio = std::clamp(runtime.localVelocity.x / walkSpeed, -1.5f, 1.5f);
    float turnRatio = 0.0f;
    if (runtime.hasPreviousYaw && dt > 0.0f) {
        float yawDelta = pc.yaw - runtime.previousYaw;
        while (yawDelta > 180.0f) yawDelta -= 360.0f;
        while (yawDelta < -180.0f) yawDelta += 360.0f;
        turnRatio = std::clamp((yawDelta / dt) / 180.0f, -1.0f, 1.0f);
    }
    runtime.previousYaw = pc.yaw;
    runtime.hasPreviousYaw = true;
    const float rollTarget = -(strafeRatio * vm.strafeRoll) - (turnRatio * vm.turnRoll);
    runtime.roll = smoothTo(runtime.roll, rollTarget, vm.rollSmoothing, dt);

    if (landed) {
        const float impact = std::clamp(landingImpactSpeed / 12.0f, 0.0f, 1.5f);
        runtime.landingDipVelocity -=
            impact * vm.landingDip * std::sqrt(std::max(1.0f, vm.landingStiffness));
    }

    const float crouchTarget = -vm.jumpCrouchDip * jumpChargeRatio;
    runtime.jumpCrouch = smoothTo(runtime.jumpCrouch, crouchTarget, 18.0f, dt);
    if (jumpLaunched) {
        runtime.landingDipVelocity +=
            -runtime.jumpCrouch * std::sqrt(std::max(1.0f, vm.landingStiffness)) * 1.5f;
        runtime.jumpCrouch = 0.0f;
    }

    springStep(runtime.landingDip, runtime.landingDipVelocity,
               vm.landingStiffness, vm.landingDamping, dt);

    outAngleOffsetDeg = glm::vec3(runtime.lookSwayOffset.x + idlePitchDeg + runLeanDeg,
                                  runtime.lookSwayOffset.y,
                                  runtime.roll + bobRollDeg);
    playerViewMotionOffset = glm::vec3(bobLateral,
                                       bobVertical + idleVertical + runtime.landingDip +
                                           runtime.jumpCrouch,
                                       0.0f);

    if (const SceneObject* cameraObject = findPlayerCameraObject()) {
        for (int walk = cameraObject->id; walk != -1;) {
            if (walk == player.id) {
                playerViewMotionCameraId = cameraObject->id;
                break;
            }
            const SceneObject* node = findObjectById(walk);
            walk = node ? node->parentId : -1;
        }
    }

    glm::vec2 swayDeg(0.0f);
    if (vm.attachedSwayEnabled) {
        runtime.attachedSwayVelocity +=
            glm::vec2(-lookAngleDelta.x, -lookAngleDelta.y) * vm.attachedSway;
        springStep(runtime.attachedSwayOffset.x, runtime.attachedSwayVelocity.x,
                   vm.attachedStiffness, vm.attachedDamping, dt);
        springStep(runtime.attachedSwayOffset.y, runtime.attachedSwayVelocity.y,
                   vm.attachedStiffness, vm.attachedDamping, dt);
        const float attachedMax = std::max(0.0f, vm.attachedMax);
        runtime.attachedSwayOffset.x = std::clamp(runtime.attachedSwayOffset.x, -attachedMax, attachedMax);
        runtime.attachedSwayOffset.y = std::clamp(runtime.attachedSwayOffset.y, -attachedMax, attachedMax);
        swayDeg = runtime.attachedSwayOffset;
    } else {
        runtime.attachedSwayOffset = glm::vec2(0.0f);
        runtime.attachedSwayVelocity = glm::vec2(0.0f);
    }

    // Held item: what an arm carrying a flashlight does. It rides the camera's own bob so it
    // stays in the hand instead of floating in front of the face, then adds a figure-eight that
    // trails the stride, a slow breath at rest, a tuck while sprinting and lag through a jump.
    // Positions are units and angles degrees (pitch, yaw, roll), both in the player's frame.
    glm::vec3 heldPos(0.0f);
    glm::vec3 heldRotDeg(0.0f);
    glm::vec3 tuckPos(0.0f);
    glm::vec3 tuckRotDeg(0.0f);
    if (vm.heldItemEnabled) {
        const float bobScale = std::max(0.0f, vm.heldItemBob);
        const float idleScale = std::max(0.0f, vm.heldItemIdle);
        const float jumpScale = std::max(0.0f, vm.heldItemJump);

        const float armPhase = runtime.bobPhase - 0.9f;
        const float swing = std::sin(armPhase);
        const float step = std::sin(armPhase * 2.0f);
        const float stride = bobWeight * bobScale;
        heldPos += glm::vec3(swing * 0.02f, step * 0.012f, 0.0f) * stride;
        heldRotDeg += glm::vec3(step * 1.5f, swing * 2.0f, -swing * 3.0f) * stride;
        heldPos.x -= strafeRatio * 0.012f * bobScale;

        // Detuned from the camera's breath so the hand and the head drift apart.
        const float breathA = std::sin(runtime.idlePhase * kTau * vm.idleFrequency * 0.83f + 0.7f);
        const float breathB = std::sin(runtime.idlePhase * kTau * vm.idleFrequency * 0.41f + 2.1f);
        const float rest = idleWeight * idleScale;
        heldPos += glm::vec3(breathB * 0.003f, breathA * 0.005f, 0.0f) * rest;
        heldRotDeg += glm::vec3(breathA * 0.8f, breathB * 0.6f, breathB * 0.7f) * rest;

        // Airborne it lags the body: sinks while rising, floats while falling. Take-off and
        // landing kick a spring, so it drops and settles back rather than snapping.
        const float airTarget = runtime.wasGrounded
            ? 0.0f
            : std::clamp(-pc.verticalVelocity * 0.004f, -0.045f, 0.045f);
        runtime.heldAirLift = smoothTo(runtime.heldAirLift, airTarget, 9.0f, dt);
        if (jumpLaunched) {
            runtime.heldDipVelocity -= 0.3f;
            runtime.heldTiltVelocity -= 25.0f;
        }
        if (landed) {
            const float impact = std::clamp(landingImpactSpeed / 12.0f, 0.0f, 1.5f);
            runtime.heldDipVelocity -= 0.55f * impact;
            runtime.heldTiltVelocity -= 55.0f * impact;
        }
        springStep(runtime.heldDip, runtime.heldDipVelocity, 110.0f, 10.0f, dt);
        springStep(runtime.heldTilt, runtime.heldTiltVelocity, 90.0f, 9.0f, dt);
        heldPos.y += (runtime.heldAirLift + runtime.heldDip) * jumpScale;
        heldRotDeg.x += (runtime.heldAirLift * 90.0f + runtime.heldTilt) * jumpScale;

        // Authored for a right-hand item; mirrored per child for one held on the left.
        const float runPose = std::max(0.0f, vm.heldItemRunPose) * runWeight;
        tuckPos = glm::vec3(-0.025f, -0.04f, 0.02f) * runPose;
        tuckRotDeg = glm::vec3(-8.0f, 5.0f, 7.0f) * runPose;

        if (playerViewMotionCameraId != -1) {
            heldPos += playerViewMotionOffset;
        }
    } else {
        runtime.heldAirLift = 0.0f;
        runtime.heldDip = 0.0f;
        runtime.heldDipVelocity = 0.0f;
        runtime.heldTilt = 0.0f;
        runtime.heldTiltVelocity = 0.0f;
    }

    // A light inside the child (the usual flashlight: a model with its spot light parented in)
    // or a light on a child that also draws a mesh. A bare light is swayed but not "held".
    const auto subtreeHasLight = [this](const SceneObject& node, int depth, const auto& self) -> bool {
        if (depth > 32) return false;
        for (int id : node.childIds) {
            const SceneObject* descendant = findObjectById(id);
            if (!descendant) continue;
            if (descendant->hasLight || self(*descendant, depth + 1, self)) return true;
        }
        return false;
    };
    const auto carriesLight = [&](const SceneObject& child) {
        if (child.hasLight && child.hasRenderer && child.renderType != RenderType::None) return true;
        return subtreeHasLight(child, 0, subtreeHasLight);
    };
    const auto safeScale = [](float s) { return std::abs(s) > 1e-4f ? s : 1.0f; };
    const glm::vec3 parentScale(safeScale(player.scale.x), safeScale(player.scale.y),
                                safeScale(player.scale.z));

    // Every offset is a rotation about the PLAYER's axes composed onto the child's authored
    // rotation. Adding degrees to the child's Euler angles instead turns about whatever axes
    // its own rotation leaves behind: a torch modelled lying down (X = 270) rolled around the
    // view axis when the mouse moved sideways instead of swinging with it.
    for (int childId : player.childIds) {
        SceneObject* child = findObjectById(childId);
        if (!child) continue;
        if (child->hasCamera && child->camera.type == SceneCameraType::Player) continue;
        const bool held = vm.heldItemEnabled && carriesLight(*child);
        auto base = runtime.attachedBase.find(childId);
        if (!vm.attachedSwayEnabled && !held) {
            if (base != runtime.attachedBase.end()) {
                child->localRotation = base->second.localRotation;
                if (base->second.movedPosition) child->localPosition = base->second.localPosition;
                child->localInitialized = true;
                runtime.attachedBase.erase(base);
            }
            continue;
        }
        if (base == runtime.attachedBase.end()) {
            PlayerControllerRuntimeState::AttachedChildBase captured;
            captured.localPosition = child->localPosition;
            captured.localRotation = child->localRotation;
            base = runtime.attachedBase.emplace(childId, captured).first;
        }

        glm::vec3 rotDeg(swayDeg.x, swayDeg.y, 0.0f);
        if (held) {
            const float side = (base->second.localPosition.x < 0.0f) ? -1.0f : 1.0f;
            rotDeg += heldRotDeg + tuckRotDeg * glm::vec3(1.0f, side, side);
            const glm::vec3 offset = heldPos + tuckPos * glm::vec3(side, 1.0f, 1.0f);
            child->localPosition = base->second.localPosition + offset / parentScale;
            base->second.movedPosition = true;
        } else if (base->second.movedPosition) {
            child->localPosition = base->second.localPosition;
            base->second.movedPosition = false;
        }

        const glm::quat offsetRot =
            glm::angleAxis(glm::radians(rotDeg.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::angleAxis(glm::radians(rotDeg.x), glm::vec3(1.0f, 0.0f, 0.0f)) *
            glm::angleAxis(glm::radians(rotDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::quat localRot = offsetRot * QuatFromEulerXYZ(base->second.localRotation);
        child->localRotation = ChooseContinuousEulerDegrees(
            glm::degrees(ExtractEulerXYZ(glm::mat3_cast(localRot), &child->localRotation)),
            child->localRotation);
        child->localInitialized = true;
    }
}

void Engine::updateRigidbody2D(float delta) {
    if (delta <= 0.0f) return;
    const bool profileEnabled = gRuntime2DProfileEnabled;
    uint64_t profileCollisionTests = 0;
    Runtime2DClock::time_point broadphaseStart;
    Runtime2DClock::time_point broadphaseEnd;
    Runtime2DClock::time_point narrowphaseStart;
    Runtime2DClock::time_point narrowphaseEnd;

    refreshSceneObjectIndexCache();
    const float gravity = -9.81f * std::max(0.0f, projectManager.currentProject.physicsSettings.globalGravityScale);
    const float minEdgeThickness = 0.01f;
    struct UiHierarchyCache {
        const std::vector<SceneObject>& objects;
        const std::unordered_map<int, size_t>& indexById;
        std::unordered_map<int, glm::vec2> worldPositionCache;

        UiHierarchyCache(const std::vector<SceneObject>& objects,
                         const std::unordered_map<int, size_t>& indexById)
            : objects(objects), indexById(indexById) {
            worldPositionCache.reserve(objects.size());
        }

        glm::vec2 getWorldPosition(const SceneObject& obj) {
            if (!(obj.hasUI && obj.ui.type != UIElementType::None)) {
                return glm::vec2(obj.position.x, obj.position.y);
            }

            auto cached = worldPositionCache.find(obj.id);
            if (cached != worldPositionCache.end()) {
                return cached->second;
            }

            glm::vec2 pos(obj.ui.position.x, obj.ui.position.y);
            if (obj.parentId >= 0) {
                auto it = indexById.find(obj.parentId);
                if (it != indexById.end()) {
                    pos += getWorldPosition(objects[it->second]);
                }
            }

            worldPositionCache.emplace(obj.id, pos);
            return pos;
        }

        glm::vec2 getParentOffset(const SceneObject& obj) {
            if (obj.parentId < 0) {
                return glm::vec2(0.0f);
            }

            auto it = indexById.find(obj.parentId);
            if (it == indexById.end()) {
                return glm::vec2(0.0f);
            }
            return getWorldPosition(objects[it->second]);
        }
    };
    UiHierarchyCache uiHierarchyCache(sceneObjects, sceneObjectIndexById);
    auto rotatePoint = [](const glm::vec2& p, float c, float s) {
        return glm::vec2(p.x * c - p.y * s, p.x * s + p.y * c);
    };
    auto buildHexagon = [](float radius, std::vector<glm::vec2>& out) {
        out.clear();
        for (int i = 0; i < 6; ++i) {
            float ang = static_cast<float>(i) * (2.0f * PI / 6.0f);
            out.emplace_back(std::cos(ang) * radius, std::sin(ang) * radius);
        }
    };
    auto computeAabb = [](const std::vector<glm::vec2>& pts, glm::vec2& outMin, glm::vec2& outMax) {
        if (pts.empty()) {
            outMin = glm::vec2(0.0f);
            outMax = glm::vec2(0.0f);
            return;
        }
        outMin = pts[0];
        outMax = pts[0];
        for (const auto& p : pts) {
            outMin.x = std::min(outMin.x, p.x);
            outMin.y = std::min(outMin.y, p.y);
            outMax.x = std::max(outMax.x, p.x);
            outMax.y = std::max(outMax.y, p.y);
        }
    };
    auto polyCenter = [](const std::vector<glm::vec2>& pts) {
        glm::vec2 c(0.0f);
        if (pts.empty()) return c;
        for (const auto& p : pts) c += p;
        return c / static_cast<float>(pts.size());
    };
    auto satOverlap = [&](const std::vector<glm::vec2>& a, const std::vector<glm::vec2>& b, glm::vec2& outAxis, float& outDepth) {
        if (a.size() < 3 || b.size() < 3) return false;
        auto testAxes = [&](const std::vector<glm::vec2>& poly, glm::vec2& axis, float& depth) {
            for (size_t i = 0; i < poly.size(); ++i) {
                glm::vec2 p0 = poly[i];
                glm::vec2 p1 = poly[(i + 1) % poly.size()];
                glm::vec2 edge = p1 - p0;
                glm::vec2 n = glm::normalize(glm::vec2(-edge.y, edge.x));
                float minA = FLT_MAX, maxA = -FLT_MAX;
                float minB = FLT_MAX, maxB = -FLT_MAX;
                for (const auto& p : a) {
                    float d = glm::dot(p, n);
                    minA = std::min(minA, d);
                    maxA = std::max(maxA, d);
                }
                for (const auto& p : b) {
                    float d = glm::dot(p, n);
                    minB = std::min(minB, d);
                    maxB = std::max(maxB, d);
                }
                float overlap = std::min(maxA, maxB) - std::max(minA, minB);
                if (overlap <= 0.0f) return false;
                if (overlap < depth) {
                    depth = overlap;
                    axis = n;
                }
            }
            return true;
        };
        glm::vec2 axis(0.0f);
        float depth = FLT_MAX;
        if (!testAxes(a, axis, depth)) return false;
        if (!testAxes(b, axis, depth)) return false;
        glm::vec2 dir = polyCenter(b) - polyCenter(a);
        if (glm::dot(axis, dir) < 0.0f) axis = -axis;
        outAxis = axis;
        outDepth = depth;
        return true;
    };
    auto segmentRect = [minEdgeThickness](const glm::vec2& a, const glm::vec2& b, float thickness, std::vector<glm::vec2>& out) {
        glm::vec2 dir = b - a;
        float len = glm::length(dir);
        if (len < 1e-4f) {
            out.clear();
            return;
        }
        glm::vec2 n = glm::vec2(-dir.y, dir.x) / len;
        float half = std::max(minEdgeThickness, thickness) * 0.5f;
        out.clear();
        out.push_back(a + n * half);
        out.push_back(b + n * half);
        out.push_back(b - n * half);
        out.push_back(a - n * half);
    };
    struct Body2DRef {
        int index = -1;
        bool dynamic = false;
        bool isTrigger = false;
        glm::vec2 pivotWorld = glm::vec2(0.0f);
        float rotationRad = 0.0f;
        std::vector<glm::vec2> poly;
        std::vector<std::pair<glm::vec2, glm::vec2>> segments;
        float edgeThickness = 0.01f;
        glm::vec2 aabbMin = glm::vec2(0.0f);
        glm::vec2 aabbMax = glm::vec2(0.0f);
        bool isEdge = false;
    };
    std::vector<Body2DRef> bodies;
    bodies.reserve(sceneObjects.size());
    float broadPhaseExtentSum = 0.0f;
    size_t broadPhaseExtentCount = 0;
    for (auto& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !UsesUIOnly2DPhysics(obj)) continue;
        if (obj.IsInvariable && obj.hasRigidbody2D && obj.rigidbody2D.enabled) {
            obj.rigidbody2D.velocity = glm::vec2(0.0f);
        }
        bool hasDynamic = obj.hasRigidbody2D && obj.rigidbody2D.enabled && !obj.IsInvariable;
        bool hasCollider2D = obj.hasCollider2D && obj.collider2D.enabled;
        if (!hasDynamic && !hasCollider2D) continue;

        if (hasDynamic) {
            glm::vec2 vel = obj.rigidbody2D.velocity;
            if (obj.rigidbody2D.useGravity) {
                vel.y += gravity * obj.rigidbody2D.gravityScale * delta;
            }
            float damping = std::max(0.0f, obj.rigidbody2D.linearDamping);
            if (damping > 0.0f) {
                vel -= vel * std::min(1.0f, damping * delta);
            }
            obj.ui.position += vel * delta;
            obj.rigidbody2D.velocity = vel;
        }

        Body2DRef body;
        body.index = static_cast<int>(&obj - &sceneObjects[0]);
        body.dynamic = hasDynamic;
        body.isTrigger = hasCollider2D && obj.collider2D.isTrigger;
        body.pivotWorld = uiHierarchyCache.getParentOffset(obj) + obj.ui.position;
        body.rotationRad = glm::radians(obj.ui.rotation);
        float c = std::cos(body.rotationRad);
        float s = std::sin(body.rotationRad);
        glm::vec2 size = glm::vec2(std::max(1.0f, obj.ui.size.x), std::max(1.0f, obj.ui.size.y));
        Collider2DType type = Collider2DType::Box;
        glm::vec2 boxSize = size;
        glm::vec2 colliderOffset(0.0f);
        std::vector<glm::vec2> localPoints;
        bool closed = false;
        float edgeThickness = minEdgeThickness;
        if (hasCollider2D) {
            type = obj.collider2D.type;
            boxSize = obj.collider2D.boxSize;
            colliderOffset = obj.collider2D.offset;
            if (boxSize.x <= 0.0f || boxSize.y <= 0.0f) {
                boxSize = size;
            }
            localPoints = obj.collider2D.points;
            closed = obj.collider2D.closed;
            edgeThickness = obj.collider2D.edgeThickness;
        }
        if (type == Collider2DType::Box) {
            glm::vec2 half = boxSize * 0.5f;
            localPoints = {
                glm::vec2(-half.x, -half.y) + colliderOffset,
                glm::vec2( half.x, -half.y) + colliderOffset,
                glm::vec2( half.x,  half.y) + colliderOffset,
                glm::vec2(-half.x,  half.y) + colliderOffset
            };
        } else if (type == Collider2DType::Polygon) {
            if (localPoints.empty()) {
                float radius = 0.5f * std::min(boxSize.x, boxSize.y);
                buildHexagon(radius, localPoints);
            }
        } else if (type == Collider2DType::Edge) {
            if (localPoints.size() < 2) {
                float half = boxSize.x * 0.5f;
                localPoints = {
                    glm::vec2(-half, 0.0f) + colliderOffset,
                    glm::vec2(half, 0.0f) + colliderOffset
                };
            }
        }
        if (type != Collider2DType::Box && (colliderOffset.x != 0.0f || colliderOffset.y != 0.0f)) {
            for (glm::vec2& point : localPoints) {
                point += colliderOffset;
            }
        }

        if (type == Collider2DType::Edge) {
            body.isEdge = true;
            body.edgeThickness = edgeThickness;
            for (size_t i = 0; i + 1 < localPoints.size(); ++i) {
                glm::vec2 a = rotatePoint(localPoints[i], c, s) + body.pivotWorld;
                glm::vec2 b = rotatePoint(localPoints[i + 1], c, s) + body.pivotWorld;
                body.segments.emplace_back(a, b);
            }
            if (closed && localPoints.size() > 2) {
                glm::vec2 a = rotatePoint(localPoints.back(), c, s) + body.pivotWorld;
                glm::vec2 b = rotatePoint(localPoints.front(), c, s) + body.pivotWorld;
                body.segments.emplace_back(a, b);
            }
            bool hasBounds = false;
            for (const auto& seg : body.segments) {
                const float halfThickness = std::max(minEdgeThickness, body.edgeThickness) * 0.5f;
                glm::vec2 segMin(std::min(seg.first.x, seg.second.x) - halfThickness,
                                 std::min(seg.first.y, seg.second.y) - halfThickness);
                glm::vec2 segMax(std::max(seg.first.x, seg.second.x) + halfThickness,
                                 std::max(seg.first.y, seg.second.y) + halfThickness);
                if (!hasBounds) {
                    body.aabbMin = segMin;
                    body.aabbMax = segMax;
                    hasBounds = true;
                } else {
                    body.aabbMin = glm::min(body.aabbMin, segMin);
                    body.aabbMax = glm::max(body.aabbMax, segMax);
                }
            }
        } else {
            body.poly.reserve(localPoints.size());
            for (const auto& p : localPoints) {
                body.poly.push_back(rotatePoint(p, c, s) + body.pivotWorld);
            }
            computeAabb(body.poly, body.aabbMin, body.aabbMax);
        }
        glm::vec2 bodySize = body.aabbMax - body.aabbMin;
        broadPhaseExtentSum += std::max(bodySize.x, bodySize.y);
        ++broadPhaseExtentCount;
        bodies.push_back(body);
    }

    auto applySeparation = [&](Body2DRef& body, const glm::vec2& sep, const glm::vec2& normal) {
        SceneObject& obj = sceneObjects[body.index];
        if (body.dynamic) {
            obj.ui.position += sep;
            body.pivotWorld += sep;
            if (!body.poly.empty()) {
                for (auto& p : body.poly) p += sep;
                body.aabbMin += sep;
                body.aabbMax += sep;
            }
            if (!body.segments.empty()) {
                for (auto& seg : body.segments) {
                    seg.first += sep;
                    seg.second += sep;
                }
            }
            float vn = glm::dot(obj.rigidbody2D.velocity, normal);
            if (vn < 0.0f) {
                obj.rigidbody2D.velocity -= normal * vn;
            }
        }
    };

    if (profileEnabled) {
        broadphaseStart = Runtime2DClock::now();
    }
    const float broadPhaseCellSize = std::clamp(
        broadPhaseExtentCount > 0 ? (broadPhaseExtentSum / static_cast<float>(broadPhaseExtentCount)) : 64.0f,
        16.0f,
        512.0f);
    auto makeCellKey = [](int x, int y) -> uint64_t {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(y);
    };
    struct BroadPhaseCellEntry {
        uint64_t key = 0;
        int bodyIndex = -1;
    };
    static thread_local std::vector<BroadPhaseCellEntry> broadPhaseEntries;
    static thread_local std::vector<uint64_t> candidatePairs;
    broadPhaseEntries.clear();
    candidatePairs.clear();
    broadPhaseEntries.reserve(bodies.size() * 4);
    candidatePairs.reserve(bodies.size() * 8);

    for (size_t bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex) {
        const Body2DRef& body = bodies[bodyIndex];
        int minCellX = static_cast<int>(std::floor(body.aabbMin.x / broadPhaseCellSize));
        int maxCellX = static_cast<int>(std::floor(body.aabbMax.x / broadPhaseCellSize));
        int minCellY = static_cast<int>(std::floor(body.aabbMin.y / broadPhaseCellSize));
        int maxCellY = static_cast<int>(std::floor(body.aabbMax.y / broadPhaseCellSize));
        for (int cellY = minCellY; cellY <= maxCellY; ++cellY) {
            for (int cellX = minCellX; cellX <= maxCellX; ++cellX) {
                BroadPhaseCellEntry entry;
                entry.key = makeCellKey(cellX, cellY);
                entry.bodyIndex = static_cast<int>(bodyIndex);
                broadPhaseEntries.push_back(entry);
            }
        }
    }

    std::sort(broadPhaseEntries.begin(), broadPhaseEntries.end(),
              [](const BroadPhaseCellEntry& a, const BroadPhaseCellEntry& b) {
                  if (a.key != b.key) return a.key < b.key;
                  return a.bodyIndex < b.bodyIndex;
              });

    size_t runStart = 0;
    while (runStart < broadPhaseEntries.size()) {
        size_t runEnd = runStart + 1;
        const uint64_t cellKey = broadPhaseEntries[runStart].key;
        while (runEnd < broadPhaseEntries.size() && broadPhaseEntries[runEnd].key == cellKey) {
            ++runEnd;
        }

        for (size_t i = runStart; i < runEnd; ++i) {
            for (size_t j = i + 1; j < runEnd; ++j) {
                int idxA = broadPhaseEntries[i].bodyIndex;
                int idxB = broadPhaseEntries[j].bodyIndex;
                if (idxA == idxB) continue;
                uint32_t aIndex = static_cast<uint32_t>(std::min(idxA, idxB));
                uint32_t bIndex = static_cast<uint32_t>(std::max(idxA, idxB));
                candidatePairs.push_back((static_cast<uint64_t>(aIndex) << 32) | bIndex);
            }
        }
        runStart = runEnd;
    }

    std::sort(candidatePairs.begin(), candidatePairs.end());
    candidatePairs.erase(std::unique(candidatePairs.begin(), candidatePairs.end()), candidatePairs.end());
    if (profileEnabled) {
        broadphaseEnd = Runtime2DClock::now();
        narrowphaseStart = broadphaseEnd;
        gRuntime2DProfileFrame.collisionPairCandidateCount += static_cast<uint64_t>(candidatePairs.size());
    }

    for (uint64_t pairKey : candidatePairs) {
        const size_t i = static_cast<size_t>(pairKey >> 32);
        const size_t j = static_cast<size_t>(pairKey & 0xffffffffu);
        if (i >= bodies.size() || j >= bodies.size()) continue;

        Body2DRef& a = bodies[i];
        Body2DRef& b = bodies[j];
        if (!a.dynamic && !b.dynamic) continue;
        if (a.aabbMax.x <= b.aabbMin.x || a.aabbMin.x >= b.aabbMax.x ||
            a.aabbMax.y <= b.aabbMin.y || a.aabbMin.y >= b.aabbMax.y) {
            continue;
        }

        auto polyVsPoly = [&](Body2DRef& pA, Body2DRef& pB) {
            if (pA.poly.empty() || pB.poly.empty()) return;
            if (profileEnabled) {
                ++profileCollisionTests;
            }
            glm::vec2 axis(0.0f);
            float depth = 0.0f;
            if (!satOverlap(pA.poly, pB.poly, axis, depth)) return;
            if (pA.isTrigger || pB.isTrigger) return;
            glm::vec2 sep = axis * depth;
            if (pA.dynamic && pB.dynamic) {
                applySeparation(pA, -sep * 0.5f, -axis);
                applySeparation(pB, sep * 0.5f, axis);
            } else if (pA.dynamic) {
                applySeparation(pA, -sep, -axis);
            } else if (pB.dynamic) {
                applySeparation(pB, sep, axis);
            }
        };

        auto polyVsEdge = [&](Body2DRef& polyBody, Body2DRef& edgeBody) {
            if (polyBody.poly.empty() || edgeBody.segments.empty()) return;
            std::vector<glm::vec2> rect;
            rect.reserve(4);
            for (const auto& seg : edgeBody.segments) {
                segmentRect(seg.first, seg.second, edgeBody.edgeThickness, rect);
                if (rect.size() < 3) continue;
                glm::vec2 rectMin(0.0f);
                glm::vec2 rectMax(0.0f);
                computeAabb(rect, rectMin, rectMax);
                if (polyBody.aabbMax.x <= rectMin.x || polyBody.aabbMin.x >= rectMax.x ||
                    polyBody.aabbMax.y <= rectMin.y || polyBody.aabbMin.y >= rectMax.y) {
                    continue;
                }
                if (profileEnabled) {
                    ++profileCollisionTests;
                }
                glm::vec2 axis(0.0f);
                float depth = 0.0f;
                if (!satOverlap(polyBody.poly, rect, axis, depth)) continue;
                if (polyBody.isTrigger || edgeBody.isTrigger) continue;
                glm::vec2 sep = axis * depth;
                if (polyBody.dynamic && edgeBody.dynamic) {
                    applySeparation(polyBody, -sep * 0.5f, -axis);
                    applySeparation(edgeBody, sep * 0.5f, axis);
                } else if (polyBody.dynamic) {
                    applySeparation(polyBody, -sep, -axis);
                } else if (edgeBody.dynamic) {
                    applySeparation(edgeBody, sep, axis);
                }
            }
        };

        if (!a.isEdge && !b.isEdge) {
            polyVsPoly(a, b);
        } else if (!a.isEdge && b.isEdge) {
            polyVsEdge(a, b);
        } else if (a.isEdge && !b.isEdge) {
            polyVsEdge(b, a);
        }
    }
    if (profileEnabled) {
        narrowphaseEnd = Runtime2DClock::now();
        gRuntime2DProfileFrame.broadphaseMs += Runtime2DMsSince(broadphaseStart, broadphaseEnd);
        gRuntime2DProfileFrame.narrowphaseSolveMs += Runtime2DMsSince(narrowphaseStart, narrowphaseEnd);
        gRuntime2DProfileFrame.collisionTestCount += profileCollisionTests;
    }
}

void Engine::updateCameraFollow2D(float delta) {
    if (sceneObjects.empty()) return;
    refreshSceneObjectIndexCache();
    struct UiHierarchyCache {
        const std::vector<SceneObject>& objects;
        const std::unordered_map<int, size_t>& indexById;
        std::unordered_map<int, glm::vec2> worldPositionCache;

        UiHierarchyCache(const std::vector<SceneObject>& objects,
                         const std::unordered_map<int, size_t>& indexById)
            : objects(objects), indexById(indexById) {
            worldPositionCache.reserve(objects.size());
        }

        glm::vec2 getWorldPosition(const SceneObject& obj) {
            if (!(obj.hasUI && obj.ui.type != UIElementType::None)) {
                return glm::vec2(obj.position.x, obj.position.y);
            }

            auto cached = worldPositionCache.find(obj.id);
            if (cached != worldPositionCache.end()) {
                return cached->second;
            }

            glm::vec2 pos(obj.ui.position.x, obj.ui.position.y);
            if (obj.parentId >= 0) {
                auto it = indexById.find(obj.parentId);
                if (it != indexById.end()) {
                    pos += getWorldPosition(objects[it->second]);
                }
            }

            worldPositionCache.emplace(obj.id, pos);
            return pos;
        }
    };
    UiHierarchyCache uiHierarchyCache(sceneObjects, sceneObjectIndexById);

    for (auto& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasCamera || !obj.hasCameraFollow2D || !obj.cameraFollow2D.enabled) continue;
        if (obj.cameraFollow2D.targetId < 0) continue;
        auto targetIt = sceneObjectIndexById.find(obj.cameraFollow2D.targetId);
        if (targetIt == sceneObjectIndexById.end()) continue;

        const SceneObject& target = sceneObjects[targetIt->second];
        if (!IsObjectEnabledInHierarchy(target)) continue;
        glm::vec2 desired2D = (target.hasUI && target.ui.type != UIElementType::None)
            ? uiHierarchyCache.getWorldPosition(target)
            : glm::vec2(target.position.x, target.position.y);
        desired2D += obj.cameraFollow2D.offset;
        glm::vec3 desired(desired2D.x, desired2D.y, obj.position.z);

        if (obj.cameraFollow2D.smoothTime > 0.0001f) {
            float alpha = 1.0f - std::exp(-delta / obj.cameraFollow2D.smoothTime);
            obj.position = glm::mix(obj.position, desired, alpha);
        } else {
            obj.position = desired;
        }

        if (obj.parentId == -1) {
            obj.localPosition = obj.position;
            obj.localInitialized = true;
        } else {
            auto parentIt = sceneObjectIndexById.find(obj.parentId);
            if (parentIt != sceneObjectIndexById.end()) {
                const SceneObject& parent = sceneObjects[parentIt->second];
                updateLocalFromWorld(obj,
                                     parent.position,
                                     QuatFromEulerXYZ(parent.rotation),
                                     parent.scale);
            }
        }
    }
}

void Engine::syncVideoPlayers(float delta) {
    for (SceneObject& obj : sceneObjects) {
        obj.runtimeHasAlbedoTextureOverride = false;
        obj.runtimeAlbedoTextureOverrideId = 0;
        obj.runtimeAlbedoTextureFlipX = false;
        obj.runtimeAlbedoTextureFlipY = false;
    }

    const bool runtimeVideoActive = isPlaying || specMode || testMode || playerMode;
    if (!runtimeVideoActive) {
        if (!videoPlayers.empty()) {
            clearVideoPlayers();
        }
        return;
    }

    std::unordered_set<int> activeVideoIds;
    activeVideoIds.reserve(sceneObjects.size());

    for (SceneObject& obj : sceneObjects) {

        if (!obj.hasVideoPlayer ||
            !obj.videoPlayer.enabled ||
            !HasRendererComponent(obj) ||
            obj.videoPlayer.videoPath.empty()) {
            videoPlayers.erase(obj.id);
            videoLoadFailedPaths.erase(obj.id);
            continue;
        }

        const fs::path resolvedVideoPath = resolveProjectAssetPath(obj.videoPlayer.videoPath);
        activeVideoIds.insert(obj.id);

        auto failedIt = videoLoadFailedPaths.find(obj.id);
        if (failedIt != videoLoadFailedPaths.end()) {
            if (failedIt->second == resolvedVideoPath.string()) {
                continue;
            }
            videoLoadFailedPaths.erase(failedIt);
        }

        auto& entry = videoPlayers[obj.id];
        if (!entry) {
            entry = std::make_unique<VideoPlayer>();
        }

        VideoPlayer& player = *entry;

        // Ensure audio configuration flags are set BEFORE LoadVideo so the decoder
        // initializes the correct audio path (test-tone vs. FFmpeg-decoded).
        player.SetPlayAudioFromVideo(obj.videoPlayer.playAudioFromVideo);
        player.SetSyncAudioToVideo(obj.videoPlayer.syncAudioToVideo);
        player.SetAudioSyncTolerance(obj.videoPlayer.audioSyncTolerance);
        player.SetAudioSystem(&audio, obj.id);

        const bool wasLoaded = player.IsLoaded() && player.GetLoadedPath() == resolvedVideoPath.string();
        if (!wasLoaded) {
            if (!player.LoadVideo(resolvedVideoPath.string())) {
                std::cerr << "Failed to load video for object '" << obj.name << "'";
                if (!obj.videoPlayer.videoPath.empty()) {
                    std::cerr << " (" << obj.videoPlayer.videoPath << ")";
                }
                if (!player.GetLastError().empty()) {
                    std::cerr << ": " << player.GetLastError();
                }
                std::cerr << std::endl;
                videoLoadFailedPaths[obj.id] = resolvedVideoPath.string();
                videoPlayers.erase(obj.id);
                continue;
            }
            player.SetAudioSystem(&audio, obj.id);
        }

        player.SetLoop(obj.videoPlayer.loop);
        player.SetPlaybackSpeed(obj.videoPlayer.playbackSpeed);
        player.SetPointFiltering(obj.material.textureFilter == MaterialProperties::TextureFilter::Point);

        const SceneObject* audioRouteObject = nullptr;
        if (obj.videoPlayer.routeAudioToSource) {
            int routeId = obj.videoPlayer.outputAudioSourceObjectId;
            if (routeId < 0 && obj.hasAudioSource) {
                routeId = obj.id;
            }
            if (routeId >= 0) {
                for (const SceneObject& candidate : sceneObjects) {
                    if (candidate.id == routeId && candidate.hasAudioSource) {
                        audioRouteObject = &candidate;
                        break;
                    }
                }
            }
        }
        audio.configureVideoStream(obj.id,
                                   audioRouteObject,
                                   obj.videoPlayer.videoAudioVolume,
                                   obj.videoPlayer.videoAudioMuted,
                                   obj.videoPlayer.loop,
                                   obj.videoPlayer.playbackSpeed);

        if (!IsObjectEnabledInHierarchy(obj)) {
            player.Pause();
        } else if (obj.videoPlayer.playOnAwake) {
            player.Play();
        }

        player.Update(delta);
        if (player.HasTextureOverride()) {
            obj.runtimeHasAlbedoTextureOverride = true;
            obj.runtimeAlbedoTextureOverrideId = player.GetTextureId();
            obj.runtimeAlbedoTextureFlipX = obj.videoPlayer.flipX;
            obj.runtimeAlbedoTextureFlipY = obj.videoPlayer.flipY;
        }
    }

    for (auto it = videoPlayers.begin(); it != videoPlayers.end();) {
        if (activeVideoIds.find(it->first) == activeVideoIds.end()) {
            it = videoPlayers.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = videoLoadFailedPaths.begin(); it != videoLoadFailedPaths.end();) {
        if (activeVideoIds.find(it->first) == activeVideoIds.end()) {
            it = videoLoadFailedPaths.erase(it);
        } else {
            ++it;
        }
    }
}

void Engine::clearVideoPlayers() {
    for (SceneObject& obj : sceneObjects) {
        obj.runtimeHasAlbedoTextureOverride = false;
        obj.runtimeAlbedoTextureOverrideId = 0;
        obj.runtimeAlbedoTextureFlipX = false;
        obj.runtimeAlbedoTextureFlipY = false;
    }
    videoPlayers.clear();
    videoLoadFailedPaths.clear();
}
#pragma endregion

#pragma region Runtime Animation
fs::path Engine::resolveAnimationClipPath(const std::string& storedPath) const {
    if (storedPath.empty()) return {};
    fs::path clipPath(storedPath);
    if (clipPath.is_absolute()) {
        return clipPath.lexically_normal();
    }
    if (projectManager.currentProject.isLoaded && !projectManager.currentProject.projectPath.empty()) {
        return (projectManager.currentProject.projectPath / clipPath).lexically_normal();
    }
    return clipPath.lexically_normal();
}

bool Engine::loadRuntimeAnimationClipFile(const fs::path& path, RuntimeAnimationClip& outClip) const {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    RuntimeAnimationClip loaded;
    std::string token;
    while (in >> token) {
        if (token == "moduanimateVersion") {
            int version = 0;
            in >> version;
            if (version < 1 || version > 2) return false;
        } else if (token == "name") {
            in >> std::quoted(loaded.name);
        } else if (token == "rootObjectId") {
            in >> loaded.rootObjectId;
        } else if (token == "duration") {
            in >> loaded.duration;
        } else if (token == "sampleRate") {
            in >> loaded.sampleRate;
        } else if (token == "bindingCount") {
            size_t bindingCount = 0;
            in >> bindingCount;
            loaded.bindings.reserve(bindingCount);
        } else if (token == "binding") {
            RuntimeAnimBinding binding;
            std::string targetType;
            in >> std::quoted(binding.path) >> std::quoted(targetType);

            std::string trackCountToken;
            size_t trackCount = 0;
            in >> trackCountToken >> trackCount;
            if (trackCountToken != "trackCount") return false;
            binding.tracks.reserve(trackCount);

            for (size_t ti = 0; ti < trackCount; ++ti) {
                std::string trackToken;
                in >> trackToken;
                if (trackToken != "track") return false;

                RuntimeAnimTrack track;
                int visible = 1;
                int locked = 0;
                in >> std::quoted(track.propertyId) >> visible >> locked;
                (void)visible;
                (void)locked;

                std::string keyCountToken;
                size_t keyCount = 0;
                in >> keyCountToken >> keyCount;
                if (keyCountToken != "keyCount") return false;
                track.keys.reserve(keyCount);

                for (size_t ki = 0; ki < keyCount; ++ki) {
                    std::string keyToken;
                    in >> keyToken;
                    if (keyToken != "key") return false;

                    uint64_t uid = 0;
                    int tangentMode = 0;
                    RuntimeAnimKey key;
                    in >> uid >> key.time >> key.value >> key.inTangent >> key.outTangent >> tangentMode >> key.interpolation;
                    (void)uid;
                    (void)tangentMode;
                    key.interpolation = std::clamp(key.interpolation, 0, 2);
                    track.keys.push_back(key);
                }
                std::sort(track.keys.begin(), track.keys.end(), [](const RuntimeAnimKey& a, const RuntimeAnimKey& b) {
                    return a.time < b.time;
                });
                binding.tracks.push_back(std::move(track));
            }
            loaded.bindings.push_back(std::move(binding));
        } else {
            std::string discard;
            std::getline(in, discard);
        }
    }

    if (!in.eof() && in.fail()) return false;
    loaded.duration = std::max(0.01f, loaded.duration);
    loaded.sampleRate = std::clamp(loaded.sampleRate, 1.0f, 240.0f);
    outClip = std::move(loaded);
    return true;
}

const Engine::RuntimeAnimationClip* Engine::getRuntimeAnimationClip(const std::string& storedPath) {
    if (storedPath.empty()) return nullptr;
    const fs::path absPath = resolveAnimationClipPath(storedPath);
    if (absPath.empty()) return nullptr;

    const std::string cacheKey = absPath.generic_string();
    RuntimeClipCacheEntry& entry = runtimeAnimationClipCache[cacheKey];

    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(absPath, ec);
    const bool haveStamp = !ec;
    if (entry.hasWriteTime == haveStamp &&
        (!haveStamp || entry.lastWriteTime == stamp)) {
        return entry.valid ? &entry.clip : nullptr;
    }

    RuntimeAnimationClip loaded;
    const bool ok = haveStamp && loadRuntimeAnimationClipFile(absPath, loaded);
    entry.hasWriteTime = haveStamp;
    entry.lastWriteTime = stamp;
    entry.valid = ok;
    if (ok) {
        entry.clip = std::move(loaded);
    } else {
        entry.clip = RuntimeAnimationClip{};
    }
    return entry.valid ? &entry.clip : nullptr;
}

float Engine::getAnimationDurationForObject(const SceneObject& obj) const {
    const std::string activeClipPath = AnimationGetActiveClipAssetPath(obj.animation);
    if (!activeClipPath.empty()) {
        const fs::path absPath = resolveAnimationClipPath(activeClipPath);
        auto it = runtimeAnimationClipCache.find(absPath.generic_string());
        if (it != runtimeAnimationClipCache.end() && it->second.valid) {
            return std::max(0.01f, it->second.clip.duration);
        }
    }
    return std::max(0.01f, obj.animation.clipLength);
}

bool Engine::applyRuntimeAnimatedProperty(SceneObject& obj, const std::string& propertyId, float value) {
    return AnimationBinding::WriteProperty(obj, propertyId, value);
}

void Engine::evaluateRuntimeAnimationClip(const RuntimeAnimationClip& clip, float time, int rootObjectId) {
    SceneObject* root = findObjectById(rootObjectId);
    if (!root) return;

    auto resolvePath = [&](const std::string& path) -> SceneObject* {
        if (path.empty()) return root;
        SceneObject* current = root;
        size_t start = 0;
        while (start <= path.size()) {
            size_t slash = path.find('/', start);
            std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : (slash - start));
            if (segment.empty()) {
                start = (slash == std::string::npos) ? path.size() + 1 : slash + 1;
                continue;
            }

            SceneObject* next = nullptr;
            for (int childId : current->childIds) {
                SceneObject* child = findObjectById(childId);
                if (child && child->name == segment) {
                    next = child;
                    break;
                }
            }
            if (!next) return nullptr;
            current = next;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return current;
    };

    auto sampleTrack = [](const RuntimeAnimTrack& track, float t) -> float {
        if (track.keys.empty()) return 0.0f;
        if (t <= track.keys.front().time) return track.keys.front().value;
        if (t >= track.keys.back().time) return track.keys.back().value;
        for (size_t i = 0; i + 1 < track.keys.size(); ++i) {
            const RuntimeAnimKey& a = track.keys[i];
            const RuntimeAnimKey& b = track.keys[i + 1];
            if (t < a.time || t > b.time) continue;
            float span = b.time - a.time;
            if (span <= 1e-6f) return b.value;
            float u = std::clamp((t - a.time) / span, 0.0f, 1.0f);
            if (a.interpolation == 0) {
                return a.value;
            }
            if (a.interpolation == 2) {
                float u2 = u * u;
                float u3 = u2 * u;
                float h00 = (2.0f * u3) - (3.0f * u2) + 1.0f;
                float h10 = u3 - (2.0f * u2) + u;
                float h01 = (-2.0f * u3) + (3.0f * u2);
                float h11 = u3 - u2;
                float m0 = a.outTangent * span;
                float m1 = b.inTangent * span;
                return (h00 * a.value) + (h10 * m0) + (h01 * b.value) + (h11 * m1);
            }
            return a.value + (b.value - a.value) * u;
        }
        return track.keys.back().value;
    };

    for (const RuntimeAnimBinding& binding : clip.bindings) {
        SceneObject* target = resolvePath(binding.path);
        if (!target) continue;
        for (const RuntimeAnimTrack& track : binding.tracks) {
            if (track.keys.empty()) continue;
            const float value = sampleTrack(track, time);
            applyRuntimeAnimatedProperty(*target, track.propertyId, value);
        }
    }
}

void Engine::updateRuntimeAnimations(float delta) {
    const bool runRuntimeAnimations = isPlaying || specMode || testMode;
    if (!runRuntimeAnimations || sceneObjects.empty()) return;

    for (SceneObject& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        if (!obj.hasAnimation || !obj.animation.enabled) continue;
        NormalizeAnimationClipSlots(obj.animation);
        const std::string activeClipPath = AnimationGetActiveClipAssetPath(obj.animation);
        if (activeClipPath.empty()) continue;

        if (obj.animation.runtimeClipPath != activeClipPath) {
            obj.animation.runtimeClipPath = activeClipPath;
            obj.animation.runtimeTime = 0.0f;
            obj.animation.runtimeDirection = 1.0f;
            obj.animation.runtimePaused = false;
            obj.animation.runtimePlaying = obj.animation.playOnAwake;
            obj.animation.runtimeInitialized = true;
        } else if (!obj.animation.runtimeInitialized) {
            obj.animation.runtimeTime = 0.0f;
            obj.animation.runtimeDirection = 1.0f;
            obj.animation.runtimePaused = false;
            obj.animation.runtimePlaying = obj.animation.playOnAwake;
            obj.animation.runtimeInitialized = true;
        }

        const RuntimeAnimationClip* clip = getRuntimeAnimationClip(activeClipPath);
        if (!clip) continue;

        const float duration = std::max(0.01f, clip->duration);
        obj.animation.clipLength = duration;

        if (obj.animation.runtimePlaying) {
            if (!obj.animation.runtimePaused && delta > 0.0f) {
                float speed = std::max(0.0f, obj.animation.playSpeed);
                float dir = (obj.animation.runtimeDirection < 0.0f) ? -1.0f : 1.0f;
                obj.animation.runtimeTime += delta * speed * dir;
                if (obj.animation.loop) {
                    while (obj.animation.runtimeTime < 0.0f) obj.animation.runtimeTime += duration;
                    while (obj.animation.runtimeTime > duration) obj.animation.runtimeTime -= duration;
                } else {
                    if (obj.animation.runtimeTime <= 0.0f) {
                        obj.animation.runtimeTime = 0.0f;
                        if (dir < 0.0f) obj.animation.runtimePlaying = false;
                    } else if (obj.animation.runtimeTime >= duration) {
                        obj.animation.runtimeTime = duration;
                        if (dir > 0.0f) obj.animation.runtimePlaying = false;
                    }
                }
            }

            const float evalTime = std::clamp(obj.animation.runtimeTime, 0.0f, duration);
            evaluateRuntimeAnimationClip(*clip, evalTime, obj.id);
        }
    }
}
#pragma endregion

#pragma region Skeletal Animation
namespace {
glm::vec3 quatToEulerXYZDegrees(const glm::quat& q) {
    return glm::degrees(ExtractEulerXYZ(glm::mat3_cast(glm::normalize(q))));
}
} // namespace

glm::vec3 QuatToEulerXYZDegrees(const glm::quat& q) { return quatToEulerXYZDegrees(q); }

glm::quat EulerXYZDegreesToQuat(const glm::vec3& deg) { return QuatFromEulerXYZ(deg); }

namespace {

glm::vec3 sampleVecKeys(const std::vector<ModelSceneData::AnimVecKey>& keys, float time, const glm::vec3& fallback) {
    if (keys.empty()) return fallback;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (time >= keys[i].time && time <= keys[i + 1].time) {
            float span = keys[i + 1].time - keys[i].time;
            float t = span > 0.0f ? (time - keys[i].time) / span : 0.0f;
            return glm::mix(keys[i].value, keys[i + 1].value, t);
        }
    }
    return keys.back().value;
}

glm::quat sampleQuatKeys(const std::vector<ModelSceneData::AnimQuatKey>& keys, float time, const glm::quat& fallback) {
    if (keys.empty()) return fallback;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (time >= keys[i].time && time <= keys[i + 1].time) {
            float span = keys[i + 1].time - keys[i].time;
            float t = span > 0.0f ? (time - keys[i].time) / span : 0.0f;
            return glm::slerp(keys[i].value, keys[i + 1].value, t);
        }
    }
    return keys.back().value;
}
}

void Engine::updateSkeletalAnimations(float delta) {
    for (auto& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasSkeletalAnimation || !obj.skeletal.enabled) continue;
        if (!obj.skeletal.useAnimation) continue;
        if (obj.meshPath.empty()) continue;

        std::string err;
        const ModelSceneData* sceneData = getModelLoader().loadModelSceneCached(obj.meshPath, err);
        if (!sceneData) continue;
        if (obj.skeletal.clipIndex < 0 || obj.skeletal.clipIndex >= (int)sceneData->animations.size()) continue;

        const auto& clip = sceneData->animations[obj.skeletal.clipIndex];
        double tps = clip.ticksPerSecond != 0.0 ? clip.ticksPerSecond : 25.0;
        obj.skeletal.time += delta * obj.skeletal.playSpeed;
        double timeTicks = obj.skeletal.time * tps;
        if (clip.duration > 0.0) {
            if (obj.skeletal.loop) {
                timeTicks = std::fmod(timeTicks, clip.duration);
                if (timeTicks < 0.0) timeTicks += clip.duration;
            } else {
                timeTicks = std::clamp(timeTicks, 0.0, clip.duration);
            }
        }
        float time = static_cast<float>(timeTicks);

        std::vector<int> animatedNodeIds = obj.skeletal.armatureNodeIds;
        if (animatedNodeIds.empty()) {
            animatedNodeIds = obj.skeletal.boneNodeIds;
        }

        for (int boneId : animatedNodeIds) {
            if (boneId < 0) continue;
            SceneObject* boneObj = findObjectById(boneId);
            if (!boneObj) continue;

            const ModelSceneData::AnimChannel* channel = nullptr;
            for (const auto& ch : clip.channels) {
                if (ch.nodeName == boneObj->name) {
                    channel = &ch;
                    break;
                }
            }
            if (!channel) continue;

            glm::vec3 pos = sampleVecKeys(channel->positions, time, boneObj->localPosition);
            glm::quat rot = sampleQuatKeys(channel->rotations, time, QuatFromEulerXYZ(boneObj->localRotation));
            glm::vec3 scale = sampleVecKeys(channel->scales, time, boneObj->localScale);

            boneObj->localPosition = pos;
            boneObj->localRotation = NormalizeEulerDegrees(quatToEulerXYZDegrees(rot));
            boneObj->localScale = scale;
            boneObj->localInitialized = true;
        }
    }
}

void Engine::updateSkinningMatrices() {
    for (auto& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasSkeletalAnimation || !obj.skeletal.enabled) continue;
        if (obj.skeletal.inverseBindMatrices.empty()) continue;

        glm::mat4 meshWorld = ComposeTransform(obj.position, obj.rotation, obj.scale);
        glm::mat4 invMesh = glm::inverse(meshWorld);

        size_t boneCount = obj.skeletal.inverseBindMatrices.size();
        if (obj.skeletal.finalMatrices.size() != boneCount) {
            obj.skeletal.finalMatrices.assign(boneCount, glm::mat4(1.0f));
        }

        for (size_t b = 0; b < boneCount; ++b) {
            int boneId = obj.skeletal.boneNodeIds.size() > b ? obj.skeletal.boneNodeIds[b] : -1;
            if (boneId < 0) {
                obj.skeletal.finalMatrices[b] = glm::mat4(1.0f);
                continue;
            }
            SceneObject* boneObj = findObjectById(boneId);
            if (!boneObj) continue;
            glm::mat4 boneWorld = ComposeTransform(boneObj->position, boneObj->rotation, boneObj->scale);
            obj.skeletal.finalMatrices[b] = invMesh * boneWorld * obj.skeletal.inverseBindMatrices[b];
        }
    }
}
#pragma endregion

void Engine::rebuildSkeletalBindings() {
    std::unordered_map<std::string, int> nameToId;
    nameToId.reserve(sceneObjects.size());
    for (const auto& obj : sceneObjects) {
        if (!obj.name.empty()) {
            nameToId[obj.name] = obj.id;
        }
    }

    auto resolveChildPath = [&](int rootId, const std::string& path) -> int {
        if (rootId < 0) return -1;
        if (path.empty()) return rootId;
        SceneObject* current = findObjectById(rootId);
        if (!current) return -1;

        size_t start = 0;
        while (start <= path.size()) {
            size_t slash = path.find('/', start);
            std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (segment.empty()) {
                if (slash == std::string::npos) break;
                start = slash + 1;
                continue;
            }

            SceneObject* next = nullptr;
            for (int childId : current->childIds) {
                SceneObject* child = findObjectById(childId);
                if (child && child->name == segment) {
                    next = child;
                    break;
                }
            }
            if (!next) return -1;
            current = next;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return current ? current->id : -1;
    };

    for (auto& obj : sceneObjects) {
        if (!obj.hasRenderer || obj.renderType != RenderType::Model || obj.meshId < 0) continue;
        const auto* meshInfo = getModelLoader().getMeshInfo(obj.meshId);
        if (!meshInfo || !meshInfo->isSkinned) continue;

        if (!obj.hasSkeletalAnimation) {
            obj.skeletal = SkeletalAnimationComponent{};
            obj.skeletal.useAnimation = false;
            obj.hasSkeletalAnimation = true;
        }
        obj.skeletal.skeletonRootId = obj.parentId;
        obj.skeletal.boneNames = meshInfo->boneNames;
        obj.skeletal.inverseBindMatrices = meshInfo->inverseBindMatrices;
        obj.skeletal.finalMatrices.assign(meshInfo->boneNames.size(), glm::mat4(1.0f));
        obj.skeletal.boneNodeIds.assign(meshInfo->boneNames.size(), -1);
        obj.skeletal.armatureNodeIds.clear();
        for (size_t b = 0; b < meshInfo->boneNames.size(); ++b) {
            if (b < meshInfo->boneNodePaths.size() && !meshInfo->boneNodePaths[b].empty()) {
                int resolvedId = resolveChildPath(obj.skeletal.skeletonRootId, meshInfo->boneNodePaths[b]);
                if (resolvedId >= 0) {
                    obj.skeletal.boneNodeIds[b] = resolvedId;
                    continue;
                }
            }
            auto it = nameToId.find(meshInfo->boneNames[b]);
            if (it != nameToId.end()) {
                obj.skeletal.boneNodeIds[b] = it->second;
            }
        }
        if (obj.skeletal.skeletonRootId >= 0) {
            std::vector<int> stack;
            stack.push_back(obj.skeletal.skeletonRootId);
            while (!stack.empty()) {
                int currentId = stack.back();
                stack.pop_back();
                SceneObject* node = findObjectById(currentId);
                if (!node) continue;
                if (node->type == ObjectType::Empty) {
                    obj.skeletal.armatureNodeIds.push_back(node->id);
                }
                for (int childId : node->childIds) {
                    if (childId >= 0) {
                        stack.push_back(childId);
                    }
                }
            }
        }
    }
}

#pragma region Transform Hierarchy
void Engine::updateLocalFromWorld(SceneObject& obj, const glm::vec3& parentPos, const glm::quat& parentRot, const glm::vec3& parentScale) {
    auto safeDiv = [](float v, float d) { return (std::abs(d) > 1e-6f) ? (v / d) : 0.0f; };
    auto unwrapNear = [](float angle, float reference) {
        float result = angle;
        while (result - reference > 180.0f) result -= 360.0f;
        while (reference - result > 180.0f) result += 360.0f;
        return result;
    };

    glm::quat invParent = glm::inverse(parentRot);
    glm::vec3 localPos = invParent * (obj.position - parentPos);
    localPos.x = safeDiv(localPos.x, parentScale.x);
    localPos.y = safeDiv(localPos.y, parentScale.y);
    localPos.z = safeDiv(localPos.z, parentScale.z);

    glm::quat worldRot = QuatFromEulerXYZ(obj.rotation);
    glm::quat localRot = invParent * worldRot;
    glm::vec3 refRot = obj.localInitialized ? obj.localRotation : obj.rotation;
    glm::vec3 localRotDeg = ChooseContinuousEulerDegrees(
        glm::degrees(ExtractEulerXYZ(glm::mat3_cast(localRot), &refRot)), refRot);
    localRotDeg.x = unwrapNear(localRotDeg.x, refRot.x);
    localRotDeg.y = unwrapNear(localRotDeg.y, refRot.y);
    localRotDeg.z = unwrapNear(localRotDeg.z, refRot.z);

    glm::vec3 localScale(1.0f);
    localScale.x = safeDiv(obj.scale.x, parentScale.x);
    localScale.y = safeDiv(obj.scale.y, parentScale.y);
    localScale.z = safeDiv(obj.scale.z, parentScale.z);

    obj.localPosition = localPos;
    obj.localRotation = localRotDeg;
    obj.localScale = localScale;
    obj.localInitialized = true;
}

void Engine::initializeLocalTransformsFromWorld(int sceneVersion) {
    if (sceneObjects.empty()) return;

    if (sceneVersion >= 10) {
        for (auto& obj : sceneObjects) {
            if (!obj.localInitialized) {
                obj.localPosition = obj.position;
                obj.localRotation = NormalizeEulerDegrees(obj.rotation);
                obj.localScale = obj.scale;
                obj.localInitialized = true;
            }
        }
        updateHierarchyWorldTransforms(true);
        return;
    }

    std::unordered_map<int, glm::mat4> worldById;
    worldById.reserve(sceneObjects.size());
    for (const auto& obj : sceneObjects) {
        worldById[obj.id] = ComposeTransform(obj.position, obj.rotation, obj.scale);
    }

    for (auto& obj : sceneObjects) {
        if (obj.parentId == -1) {
            obj.localPosition = obj.position;
            obj.localRotation = NormalizeEulerDegrees(obj.rotation);
            obj.localScale = obj.scale;
            obj.localInitialized = true;
            continue;
        }
        auto itParent = worldById.find(obj.parentId);
        if (itParent == worldById.end()) {
            obj.localPosition = obj.position;
            obj.localRotation = NormalizeEulerDegrees(obj.rotation);
            obj.localScale = obj.scale;
            obj.localInitialized = true;
            continue;
        }
        glm::vec3 pPos, pRotDeg, pScale;
        DecomposeMatrix(itParent->second, pPos, pRotDeg, pScale);
        updateLocalFromWorld(obj, pPos, QuatFromEulerXYZ(pRotDeg), pScale);
    }

    updateHierarchyWorldTransforms(true);
}

void Engine::refreshSceneObjectIndexCache() {
    const SceneObject* currentData = sceneObjects.empty() ? nullptr : sceneObjects.data();
    if (sceneObjectIndexData == currentData &&
        sceneObjectIndexCount == sceneObjects.size() &&
        sceneObjectIndexById.size() == sceneObjects.size()) {
        if (sceneObjectIndexVerifiedFrame == renderFrameSerial) {
            return;
        }
        bool cacheMatchesCurrentObjects = true;
        for (size_t i = 0; i < sceneObjects.size(); ++i) {
            auto it = sceneObjectIndexById.find(sceneObjects[i].id);
            if (it == sceneObjectIndexById.end() || it->second != i) {
                cacheMatchesCurrentObjects = false;
                break;
            }
        }
        if (cacheMatchesCurrentObjects) {
            sceneObjectIndexVerifiedFrame = renderFrameSerial;
            return;
        }
    }

    sceneObjectIndexById.clear();
    sceneObjectIndexById.reserve(sceneObjects.size());
    for (size_t i = 0; i < sceneObjects.size(); ++i) {
        sceneObjectIndexById[sceneObjects[i].id] = i;
    }
    sceneObjectIndexData = currentData;
    sceneObjectIndexCount = sceneObjects.size();
    sceneObjectIndexVerifiedFrame = renderFrameSerial;

    std::vector<int> validSelectedIds;
    validSelectedIds.reserve(selectedObjectIds.size());
    for (int id : selectedObjectIds) {
        if (sceneObjectIndexById.find(id) == sceneObjectIndexById.end()) continue;
        if (std::find(validSelectedIds.begin(), validSelectedIds.end(), id) != validSelectedIds.end()) continue;
        validSelectedIds.push_back(id);
    }
    selectedObjectIds = std::move(validSelectedIds);
    if (selectedObjectId >= 0 && sceneObjectIndexById.find(selectedObjectId) == sceneObjectIndexById.end()) {
        selectedObjectId = selectedObjectIds.empty() ? -1 : selectedObjectIds.back();
    }
    if (hierarchyRangeAnchorId >= 0 && sceneObjectIndexById.find(hierarchyRangeAnchorId) == sceneObjectIndexById.end()) {
        hierarchyRangeAnchorId = selectedObjectId;
    }
}

void Engine::rebuildRuntimeScriptBindings() {
    runtimeScriptBindings.clear();
    runtimeScriptBindings.reserve(sceneObjects.size());
    for (const auto& obj : sceneObjects) {
        if (obj.scripts.empty()) continue;
        for (size_t i = 0; i < obj.scripts.size(); ++i) {
            runtimeScriptBindings.push_back({obj.id, i});
        }
    }
    runtimeScriptBindingsCachedVersion = runtimeScriptBindingsVersion;
}

void Engine::updateHierarchyWorldTransforms(bool rebuildWorldFromLocal) {
    if (sceneObjects.empty()) return;
    refreshSceneObjectIndexCache();

    bool hasHierarchyLinks = false;
    for (const auto& obj : sceneObjects) {
        if (obj.parentId != -1 || !obj.childIds.empty()) {
            hasHierarchyLinks = true;
            break;
        }
    }

    if (!hasHierarchyLinks) {
        const glm::vec3 rootPos(0.0f);
        const glm::quat rootRot(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::vec3 rootScale(1.0f);
        for (auto& obj : sceneObjects) {
            obj.hierarchyEnabled = true;
            if (!obj.localInitialized) {
                obj.localPosition = obj.position;
                obj.localRotation = NormalizeEulerDegrees(obj.rotation);
                obj.localScale = obj.scale;
                obj.localInitialized = true;
            }

            bool useWorldAuthoritative = !rebuildWorldFromLocal &&
                obj.hasRigidbody && obj.rigidbody.enabled && !obj.rigidbody.isKinematic &&
                !obj.IsInvariable;
            if (useWorldAuthoritative) {
                updateLocalFromWorld(obj, rootPos, rootRot, rootScale);
                continue;
            }

            obj.position = obj.localPosition;
            obj.rotation = NormalizeEulerDegrees(obj.localRotation);
            obj.scale = obj.localScale;
        }
        return;
    }

    auto unwrapNear = [](float angle, float reference) {
        float result = angle;
        while (result - reference > 180.0f) result -= 360.0f;
        while (reference - result > 180.0f) result += 360.0f;
        return result;
    };

    std::unordered_set<int> visiting;
    std::unordered_set<int> visited;
    visiting.reserve(sceneObjects.size());
    visited.reserve(sceneObjects.size());

    std::function<void(int, const glm::vec3&, const glm::quat&, const glm::vec3&, bool)> processNode =
        [&](int id,
            const glm::vec3& parentPos,
            const glm::quat& parentRot,
            const glm::vec3& parentScale,
            bool parentHierarchyEnabled) {
        if (visited.count(id)) return;
        if (visiting.count(id)) return;
        auto itIndex = sceneObjectIndexById.find(id);
        if (itIndex == sceneObjectIndexById.end()) return;

        visiting.insert(id);
        SceneObject& obj = sceneObjects[itIndex->second];
        obj.hierarchyEnabled = parentHierarchyEnabled;
        if (!obj.localInitialized) {
            obj.localPosition = obj.position;
            obj.localRotation = NormalizeEulerDegrees(obj.rotation);
            obj.localScale = obj.scale;
            obj.localInitialized = true;
        }

        bool useWorldAuthoritative = !rebuildWorldFromLocal &&
            obj.hasRigidbody && obj.rigidbody.enabled && !obj.rigidbody.isKinematic &&
            !obj.IsInvariable;
        glm::vec3 worldPos = obj.position;
        glm::quat worldRot = QuatFromEulerXYZ(obj.rotation);
        glm::vec3 worldScale = obj.scale;
        if (useWorldAuthoritative) {
            updateLocalFromWorld(obj, parentPos, parentRot, parentScale);
            worldPos = obj.position;
            worldRot = QuatFromEulerXYZ(obj.rotation);
            worldScale = obj.scale;
        } else if (obj.parentId == -1) {
            obj.position = obj.localPosition;
            obj.rotation = NormalizeEulerDegrees(obj.localRotation);
            obj.scale = obj.localScale;
            worldPos = obj.position;
            worldRot = QuatFromEulerXYZ(obj.rotation);
            worldScale = obj.scale;
        } else {
            glm::quat localRot = QuatFromEulerXYZ(obj.localRotation);
            worldRot = parentRot * localRot;
            worldScale = parentScale * obj.localScale;
            worldPos = parentPos + parentRot * (parentScale * obj.localPosition);
            glm::vec3 worldRotDeg = ChooseContinuousEulerDegrees(
                glm::degrees(ExtractEulerXYZ(glm::mat3_cast(worldRot), &obj.rotation)), obj.rotation);
            worldRotDeg.x = unwrapNear(worldRotDeg.x, obj.rotation.x);
            worldRotDeg.y = unwrapNear(worldRotDeg.y, obj.rotation.y);
            worldRotDeg.z = unwrapNear(worldRotDeg.z, obj.rotation.z);
            obj.position = worldPos;
            obj.rotation = worldRotDeg;
            obj.scale = worldScale;
        }

        const bool childParentHierarchyEnabled = IsObjectEnabledInHierarchy(obj);
        for (int childId : obj.childIds) {
            processNode(childId, worldPos, worldRot, worldScale, childParentHierarchyEnabled);
        }

        visiting.erase(id);
        visited.insert(id);
    };

    for (const auto& obj : sceneObjects) {
        if (obj.parentId == -1 || sceneObjectIndexById.find(obj.parentId) == sceneObjectIndexById.end()) {
            processNode(obj.id,
                        glm::vec3(0.0f),
                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                        glm::vec3(1.0f),
                        true);
        }
    }
}
#pragma endregion

#pragma region Project Lifecycle
void Engine::setStartupProjectPath(const std::string& path) {
    startupProjectPath = path;
}

void Engine::OpenProjectPath(const std::string& path) {
    startProjectLoad(path);
}

void Engine::traceProjectLoad(const char* tag) {
    if (!projectLoadTraceActive) return;
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - projectLoadTraceOrigin).count();
    std::fprintf(stderr, "[LoadTrace] %8.1f ms  %s\n", ms, tag);
    std::fflush(stderr);
}

void Engine::startProjectLoad(const std::string& path) {
    if (projectLoadInProgress) return;
#if !MODULARITY_RUNTIME_ONLY
    openProjectInVR = false;
#endif
    projectLoadTraceOrigin = std::chrono::steady_clock::now();
    projectLoadTraceActive = true;
    traceProjectLoad("startProjectLoad: enter");
    projectManager.errorMessage.clear();
    projectLoadInProgress = true;
    projectLoadStartTime = glfwGetTime();
    projectLoadPath = path;
    showLauncher = true;
    launcherTransitionPendingHide = false;
    launcherLoadingPreviewPath.clear();
    fs::path previewPath = getProjectPreviewPath(path);
    if (!previewPath.empty() && fs::exists(previewPath)) {
        launcherLoadingPreviewPath = previewPath.string();
    }
    traceProjectLoad("startProjectLoad: preview resolved");

    projectLoadFuture = std::async(std::launch::async, [path]() {
        ProjectLoadResult result;
        result.path = path;
        try {
            Project project;
            if (project.load(path)) {
                result.success = true;
                result.project = std::move(project);
            } else if (Modularity::FileContainsNulBytes(path)) {
                result.error = Modularity::kDamagedProjectFileMessage;
            } else {
                result.error = "Failed to load project file";
            }
        } catch (const std::exception& e) {
            result.error = std::string("Exception opening project: ") + e.what();
        } catch (...) {
            result.error = "Unknown exception opening project";
        }
        return result;
    });
    traceProjectLoad("startProjectLoad: worker dispatched");
}

void Engine::pollProjectLoad() {
    if (!projectLoadInProgress) return;
    if (!projectLoadFuture.valid()) {
        projectLoadInProgress = false;
        return;
    }

    auto state = projectLoadFuture.wait_for(std::chrono::milliseconds(0));
    if (state == std::future_status::ready) {
        traceProjectLoad("pollProjectLoad: worker finished (async project.load done)");
        ProjectLoadResult result = projectLoadFuture.get();
        projectLoadInProgress = false;
        finishProjectLoad(result);
        traceProjectLoad("pollProjectLoad: finishProjectLoad returned");
    }
}

void Engine::beginDeferredSceneLoad(const std::string& sceneName) {
    if (sceneLoadInProgress || !projectManager.currentProject.isLoaded) return;
    traceProjectLoad("beginDeferredSceneLoad: enter");

    sceneObjects.clear();
    clearSelection();
    nextObjectId = 0;
    undoStack.clear();
    redoStack.clear();

    sceneLoadInProgress = true;
    sceneLoadProgress = 0.0f;
    sceneLoadStatus = "Reading scene...";
    sceneLoadSceneName = sceneName;
    sceneLoadObjects.clear();
    sceneLoadAssetIndices.clear();
    sceneLoadAssetsDone = 0;
    sceneLoadNextId = 0;
    sceneLoadVersion = 9;
    sceneLoadMetadata = SceneSerializer::Metadata{};
    sceneLoadTimeOfDay = -1.0f;
    sceneLoadSkyboxSettings = SkyboxSettings{};
    showLauncher = true;
    projectLoadStartTime = glfwGetTime();
    launcherLoadingPreviewPath.clear();
    fs::path previewPath = getProjectPreviewPath(projectManager.currentProject.projectPath);
    if (!previewPath.empty() && fs::exists(previewPath)) {
        launcherLoadingPreviewPath = previewPath.string();
    }

    fs::path scenePath = projectManager.currentProject.getSceneFilePath(sceneName);
    if (!fs::exists(scenePath)) {
        sceneLoadInProgress = false;
        addConsoleMessage("Default scene not found, starting with a new scene.", ConsoleMessageType::Info);
        applySceneSkyboxSettings(SkyboxSettings{});
        applySceneTimeOfDay(0.5f);
        createPipelineDefaultSceneObjects();
        initializeNewSceneSerializationState(scenePath);
        showLauncher = false;
        return;
    }

    if (!SceneSerializer::loadSceneDeferred(scenePath,
                                            sceneLoadObjects,
                                            sceneLoadNextId,
                                            sceneLoadVersion,
                                            &sceneLoadTimeOfDay,
                                            &sceneLoadSkyboxSettings,
                                            &sceneLoadMetadata)) {
        sceneLoadInProgress = false;
        for (const auto& [message, type] : DescribeUnreadableScene(sceneName, scenePath, true)) {
            addConsoleMessage(message, type);
        }
        applySceneSkyboxSettings(SkyboxSettings{});
        applySceneTimeOfDay(0.5f);
        createPipelineDefaultSceneObjects();
        initializeNewSceneSerializationState(scenePath);
        // The placeholder is not work to keep. Left dirty, closing the editor auto-saves it over
        // the file that failed to load - including one the user restores while the editor is open.
        projectManager.currentProject.hasUnsavedChanges = false;
        showLauncher = false;
        return;
    }

    for (size_t i = 0; i < sceneLoadObjects.size(); ++i) {
        const auto& obj = sceneLoadObjects[i];
        if (!obj.hasRenderer) continue;
        if ((obj.renderType == RenderType::OBJMesh || obj.renderType == RenderType::Model) &&
            !obj.meshPath.empty()) {
            sceneLoadAssetIndices.push_back(i);
        }
    }

    if (sceneLoadAssetIndices.empty()) {
        sceneLoadProgress = 1.0f;
        sceneLoadStatus = "Finalizing scene...";
        finalizeDeferredSceneLoad();
    } else {
        sceneLoadProgress = 0.0f;
        sceneLoadStatus = "Loading scene assets...";
    }
}

void Engine::pollSceneLoad() {
    if (!sceneLoadInProgress) return;

    if (sceneLoadAssetIndices.empty()) {
        return;
    }

#if defined(_WIN32)
    constexpr size_t kAssetsPerFrame = 3;
#else
    constexpr size_t kAssetsPerFrame = 1;
#endif
    size_t processed = 0;
    while (sceneLoadAssetsDone < sceneLoadAssetIndices.size() && processed < kAssetsPerFrame) {
        size_t objIndex = sceneLoadAssetIndices[sceneLoadAssetsDone];
        SceneObject& obj = sceneLoadObjects[objIndex];
        const auto __aT0 = std::chrono::steady_clock::now();

        if (obj.renderType == RenderType::OBJMesh) {
            std::string err;
            obj.meshId = g_objLoader.loadOBJ(obj.meshPath, err);
            if (obj.meshId < 0 && !err.empty()) {
                std::cerr << "Failed to load OBJ: " << err << std::endl;
            }
        } else if (obj.renderType == RenderType::Model) {
            ModelSceneData sceneData;
            std::string err;
            if (getModelLoader().loadModelScene(obj.meshPath, sceneData, err)) {
                int sourceIndex = obj.meshSourceIndex;
                if (sourceIndex < 0 || sourceIndex >= (int)sceneData.meshIndices.size()) {
                    sourceIndex = 0;
                }
                if (!sceneData.meshIndices.empty() &&
                    sourceIndex >= 0 && sourceIndex < (int)sceneData.meshIndices.size()) {
                    obj.meshId = sceneData.meshIndices[sourceIndex];
                }
                ApplyModelRootTransform(obj, sceneData);
            } else {
                std::cerr << "Failed to load model from scene: " << err << std::endl;
                obj.meshId = -1;
            }
        }

        const double __aMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - __aT0).count();
        if (__aMs > Modularity::HardwareProfile::TimingBudget(30.0)) {
            std::fprintf(stderr, "[ModuTimer] sceneAsset %.1f ms  %s\n", __aMs, obj.meshPath.c_str());
        }
        ++sceneLoadAssetsDone;
        ++processed;
    }

    float total = static_cast<float>(sceneLoadAssetIndices.size());
    sceneLoadProgress = total > 0.0f ? (static_cast<float>(sceneLoadAssetsDone) / total) : 1.0f;
    sceneLoadStatus = "Loading scene assets (" + std::to_string(sceneLoadAssetsDone) + "/" +
                      std::to_string(sceneLoadAssetIndices.size()) + ")";

    if (sceneLoadAssetsDone >= sceneLoadAssetIndices.size()) {
        sceneLoadStatus = "Finalizing scene...";
        finalizeDeferredSceneLoad();
    }
}

void Engine::finalizeDeferredSceneLoad() {
    if (!sceneLoadInProgress) return;
    const auto __fdT0 = std::chrono::steady_clock::now();

    sceneObjects = std::move(sceneLoadObjects);
    ResetParticleSystem2DRuntimes(sceneObjects);
    nextObjectId = sceneLoadNextId;
    const auto __fdT1 = std::chrono::steady_clock::now();

    initializeLocalTransformsFromWorld(sceneLoadVersion);
    const auto __fdT2 = std::chrono::steady_clock::now();
    rebuildSkeletalBindings();
    const auto __fdT3 = std::chrono::steady_clock::now();
    currentSceneSerialization = sceneLoadMetadata;
    legacySceneSaveChoice = sceneLoadMetadata.fileFormat == SceneSerializer::FileFormat::LegacyFlat
        ? LegacySceneSaveChoice::Ask
        : LegacySceneSaveChoice::SaveModular;

    projectManager.currentProject.currentSceneName = sceneLoadSceneName;
    projectManager.currentProject.hasUnsavedChanges = false;
    projectManager.currentProject.saveProjectFile();
    clearSelection();

    recordState("sceneLoaded");
    addConsoleMessage("Loaded scene: " + sceneLoadSceneName, ConsoleMessageType::Success);
    applySceneSkyboxSettings(sceneLoadSkyboxSettings);
    applySceneTimeOfDay(sceneLoadTimeOfDay >= 0.0f ? sceneLoadTimeOfDay : 0.5f);

    // Deferred loading can complete after play mode was already entered.
    // Rebuild runtime systems so physics/audio always match the loaded scene.
    if (isPlaying || specMode || testMode) {
        updateHierarchyWorldTransforms();
        physics->onPlayStart(sceneObjects);
        audio.setPrefer2DSpatialAudio(isProject2DPipeline() || uiWorldMode);
        audio.onPlayStart(sceneObjects);
        if (playerMode) {
            syncPlayerCamera();
        }
    }

    sceneLoadInProgress = false;
    sceneLoadProgress = 1.0f;
    sceneLoadStatus.clear();
    sceneLoadAssetIndices.clear();
    if (launcherTransitionActive) {
        launcherTransitionPendingHide = true;
        showLauncher = true;
    } else {
        showLauncher = false;
    }
    const auto __fdT4 = std::chrono::steady_clock::now();
    auto msF = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::fprintf(stderr,
        "[ModuTimer] finalizeDeferredSceneLoad: move+particles=%.1f xforms=%.1f skel=%.1f tail=%.1f total=%.1f\n",
        msF(__fdT0, __fdT1), msF(__fdT1, __fdT2), msF(__fdT2, __fdT3), msF(__fdT3, __fdT4), msF(__fdT0, __fdT4));
    traceProjectLoad("finalizeDeferredSceneLoad: scene ready (project usable)");
    projectLoadTraceActive = false;
}

#ifndef __ANDROID__
// Project Settings > Player stores a startup size and fullscreen flag, but nothing applied them, so
// every exported game opened at Window.cpp's hard-coded 1000x800. The size is the client area; it
// is clamped so the window, frame included, fits the monitor's work area.
static void ApplyPlayerStartupWindow(GLFWwindow* window, const ProjectPlayerSettings& player) {
    if (!window) return;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (!monitor || !mode) return;

    if (player.fullscreenStartup) {
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        return;
    }

    int areaX = 0, areaY = 0, areaW = mode->width, areaH = mode->height;
    glfwGetMonitorWorkarea(monitor, &areaX, &areaY, &areaW, &areaH);
    int frameLeft = 0, frameTop = 0, frameRight = 0, frameBottom = 0;
    glfwGetWindowFrameSize(window, &frameLeft, &frameTop, &frameRight, &frameBottom);
    const int maxWidth = std::max(64, areaW - frameLeft - frameRight);
    const int maxHeight = std::max(64, areaH - frameTop - frameBottom);
    const int width = std::min(std::clamp(player.startupWidth, 64, 8192), maxWidth);
    const int height = std::min(std::clamp(player.startupHeight, 64, 8192), maxHeight);
    glfwSetWindowSize(window, width, height);
    glfwSetWindowPos(window,
                     areaX + frameLeft + (maxWidth - width) / 2,
                     areaY + frameTop + (maxHeight - height) / 2);
}
#endif

void Engine::finishProjectLoad(ProjectLoadResult& result) {
    bool inVR = false;
#if !MODULARITY_RUNTIME_ONLY
    inVR = openProjectInVR;
    openProjectInVR = false;
#endif
    const auto __fpStart = std::chrono::steady_clock::now();
    auto __fpDone = [&](const char* tag) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - __fpStart).count();
        std::fprintf(stderr, "[ModuTimer] finishProjectLoad/%s %.1f ms\n", tag, ms);
    };
    struct __Scoped { std::function<void()> f; ~__Scoped(){ if(f) f(); } } __fpScope{[&]{ __fpDone("total"); }};
    if (!result.success) {
        projectManager.errorMessage = result.error.empty() ? "Failed to load project file" : result.error;
        addConsoleMessage("Error opening project: " + projectManager.errorMessage, ConsoleMessageType::Error);
        showLauncher = true;
        return;
    }

    projectManager.currentProject = std::move(result.project);
    const ProjectConsoleSettings& consoleSettings = projectManager.currentProject.consoleSettings;
    consolePanelExpanded = consoleSettings.alwaysOpenOnLaunch;
    showConsole = consoleSettings.mode == ProjectConsoleMode::DockedMiniButton ||
                  consoleSettings.alwaysOpenOnLaunch;
    if (physics) physics->shutdown();
    physics = CreatePhysicsBackend(projectManager.currentProject.physicsSettings.backend);
    physics->setProjectSettings(projectManager.currentProject.physicsSettings);
    traceProjectLoad("  physics backend created");
    projectManager.addToRecentProjects(projectManager.currentProject.name, result.path);
    traceProjectLoad("  addToRecentProjects");
    vulkanMaterialFeatureWarningShown = false;
    packageManager.setProjectRoot(projectManager.currentProject.projectPath);
    traceProjectLoad("  packageManager.setProjectRoot");
    renderer.clearTextureFormatOverrides();
    renderer.setTextureKeyRoot(projectManager.currentProject.projectPath.string());
    for (const auto& [texPath, format] : projectManager.currentProject.textureFormatOverrides) {
        renderer.setTextureFormatOverride(texPath, TextureFormatPolicyFromString(format));
    }
    applyProjectGraphicsToRenderer();
    traceProjectLoad("  textureOverrides + applyProjectGraphicsToRenderer");
#ifndef __ANDROID__
    if (playerMode) {
        ApplyPlayerStartupWindow(editorWindow, projectManager.currentProject.playerSettings);
    }
#endif
    clampOptionalPackageState(true);
    traceProjectLoad("  clampOptionalPackageState");
    fs::path contentRoot = projectManager.currentProject.usesNewLayout
        ? projectManager.currentProject.assetsPath
        : projectManager.currentProject.projectPath;
    if (!playerMode) {
        fileBrowser.setProjectRoot(contentRoot);
        fileBrowser.currentPath = contentRoot;
    }

#ifndef __ANDROID__
    if (projectManager.currentProject.rendererBackend != graphicsBackend) {
        const Modularity::GraphicsBackend targetBackend = projectManager.currentProject.rendererBackend;
        std::string relaunchError;
        if (relaunchEditorWithBackend(targetBackend, result.path, relaunchError, inVR)) {
            addConsoleMessage(
                "Project renderer is " + std::string(Modularity::ToString(targetBackend)) +
                ". Restarting editor with matching backend...",
                ConsoleMessageType::Info);
            glfwSetWindowShouldClose(editorWindow, GLFW_TRUE);
            return;
        }

        addConsoleMessage(
            "Project renderer is " + std::string(Modularity::ToString(targetBackend)) +
            ", current session is " + std::string(Modularity::ToString(graphicsBackend)) +
            ". Automatic backend switch failed (" + relaunchError + "). Restart editor to apply project renderer.",
            ConsoleMessageType::Warning);
    }
#endif

    // Make sure project folders exist even for older/minimal projects
    if (!fs::exists(projectManager.currentProject.assetsPath)) {
        fs::create_directories(projectManager.currentProject.assetsPath);
    }
    if (!fs::exists(projectManager.currentProject.scenesPath)) {
        fs::create_directories(projectManager.currentProject.scenesPath);
    }

    __fpDone("preRenderer");
    traceProjectLoad("finishProjectLoad: physics+packages+graphics done, entering initRenderer");
    if (!initRenderer()) {
        addConsoleMessage("Error: Failed to initialize renderer!", ConsoleMessageType::Error);
        showLauncher = true;
        return;
    }

    if (!physics->isReady() && !physics->init()) {
        addConsoleMessage("Warning: PhysX failed to initialize; physics disabled for this session", ConsoleMessageType::Warning);
    }

    __fpDone("renderer+physics");
    traceProjectLoad("finishProjectLoad: renderer+physics init done");
    loadBuildSettings();
    if (autoStartRequested) {
        // Before the scene load, so the first frame finds every program already linked.
        preloadRuntimeShaders();
    }
    if (autoStartRequested && !autoStartSceneName.empty()) {
        beginDeferredSceneLoad(autoStartSceneName);
    } else {
        loadRecentScenes();
    }
    __fpDone("buildSettings+scene");
    traceProjectLoad("finishProjectLoad: buildSettings + scene dispatch done");
    refreshUIFontCatalog();
    preloadUIFontCatalogForContext(ImGui::GetCurrentContext());
    if (!playerMode) {
        fileBrowser.needsRefresh = true;
        loadEditorUserSettings();
    }
    __fpDone("fonts+userSettings");
    traceProjectLoad("finishProjectLoad: fonts + editor user settings done");
    std::thread([]() {
        ScriptCompiler::prewarmToolchainEnvironment();
    }).detach();
    applyProjectPipelineDefaults(false);
    fileBrowser.needsRefresh = !playerMode;
    scriptEditorWindowsDirty = true;
    scriptEditorWindows.clear();
    scriptEditorWindowProbeCache.clear();
    scriptEditorWindowProbeCacheLoaded = false;
    scriptEditorWindowProbeCacheDirty = false;
    scriptLastAutoCompileTime.clear();
    scriptAutoCompileCheckedSourceTime.clear();
    scriptAutoCompileBinaryCache.clear();
    scriptAutoCompileDiscoveredSources.clear();
    ++scriptAutoCompileScanGeneration;
    scriptAutoCompileConfigValid = false;
    scriptBinaryResolveCache.clear();
    scriptBuildConfigCache.clear();
    autoCompileQueue.clear();
    autoCompileQueued.clear();
    scriptAutoCompileLastCheck = 0.0;
    scriptAutoCompileLastDirectoryScan = 0.0;
    managedAutoCompileLastScan = 0.0;
    managedAutoCompileCachedProjectDir.clear();
    managedAutoCompileNewestSource = fs::file_time_type{};
    managedAutoCompileHasSource = false;
    managedAutoCompileCompiledSource = fs::file_time_type{};
    managedAutoCompileHasCompiled = false;
    if (!sceneLoadInProgress) {
        if (launcherTransitionActive) {
            launcherTransitionPendingHide = true;
            showLauncher = true;
        } else {
            showLauncher = false;
        }
    }
    #ifdef MODULARITY_PLAYER
    applyAutoStartMode();
    #else
    if (autoStartRequested && autoStartPlayerMode) {
        applyAutoStartMode();
    } else {
        playerMode = false;
        startupSplashStartTime = -1.0;
    }
    #endif
    if (playerMode) {
        syncPlayerCamera();
    }
    traceProjectLoad("finishProjectLoad: script cache reset + launcher state done");
    applyBuildWindowTitle();
    addConsoleMessage("Opened project: " + projectManager.currentProject.name, ConsoleMessageType::Info);
#if !MODULARITY_RUNTIME_ONLY && MODULARITY_HAS_OPENXR
    if (inVR && !playerMode && projectManager.currentProject.openXRSettings.enabled && !usingVulkan()) {
        vrEditor.requested = true;
        vrEditor.preview = false;
        vrEditor.recenterPending = true;
        xrStartupAttempted = false;
        gameViewCursorLocked = false;
        cursorLocked = false;
    }
#endif
    traceProjectLoad("finishProjectLoad: exit");
}

void Engine::syncPlayerCamera() {
    const SceneObject* activeCam = findPlayerCameraObject();
    if (!activeCam) {
        return;
    }
    Camera cam = makeCameraFromObject(*activeCam);
    cam.firstMouse = true;
    camera = cam;
}

void Engine::loadAutoStartConfig() {
    autoStartRequested = false;
    autoStartPlayerMode = false;
    autoStartBundlePath.clear();
    autoStartBundleMode = "extract";
    autoStartVerifyIntegrity = false;
    autoStartProjectPath.clear();
    autoStartSceneName.clear();

    fs::path configPath = fs::current_path() / "autostart.modu";
    bool configFromAsset = false;
    std::string configText;
    if (fs::exists(configPath)) {
        std::ifstream file(configPath);
        if (!file.is_open()) return;
        std::ostringstream ss;
        ss << file.rdbuf();
        configText = ss.str();
    } else if (Modularity::Platform::GetAssetSource().Exists("autostart.modu")) {
        std::vector<uint8_t> bytes =
            Modularity::Platform::GetAssetSource().ReadAll("autostart.modu");
        if (bytes.empty()) return;
        configText.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        configFromAsset = true;
    } else {
        return;
    }

    auto trim = [](std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        auto end = s.find_last_not_of(" \t\r\n");
        if (start == std::string::npos || end == std::string::npos) {
            s.clear();
            return;
        }
        s = s.substr(start, end - start + 1);
    };

    std::string line;
    bool modeSpecified = false;
    bool sawKey = false;
    bool oneShot = false;
    std::istringstream file(configText);
    while (std::getline(file, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) {
            if (!sawKey && autoStartProjectPath.empty()) {
                autoStartProjectPath = line;
            }
            continue;
        }
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        trim(key);
        trim(value);
        sawKey = true;
        if (key == "project") {
            autoStartProjectPath = value;
        } else if (key == "bundle") {
            autoStartBundlePath = value;
        } else if (key == "bundleMode") {
            autoStartBundleMode = (value == "mount") ? "mount" : "extract";
        } else if (key == "verifyIntegrity") {
            std::string lower = value;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            autoStartVerifyIntegrity = (lower == "1" || lower == "true" || lower == "yes");
        } else if (key == "scene") {
            autoStartSceneName = value;
        } else if (key == "mode") {
            autoStartPlayerMode = (value == "player");
            modeSpecified = true;
        } else if (key == "oneshot") {
            std::string lower = value;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            oneShot = (lower == "1" || lower == "true" || lower == "yes");
        }
    }

    if (!autoStartProjectPath.empty()) {
        fs::path path = autoStartProjectPath;
        if (path.is_relative() && autoStartBundlePath.empty()) {
            path = fs::current_path() / path;
        }
        autoStartProjectPath = path.lexically_normal().string();
        autoStartRequested = true;
        if (!modeSpecified) {
            autoStartPlayerMode = true;
        }
    }

    if (!autoStartBundlePath.empty()) {
        fs::path bundlePath = autoStartBundlePath;
        if (bundlePath.is_relative() && !configFromAsset) {
            bundlePath = fs::current_path() / bundlePath;
        }
        autoStartBundlePath = bundlePath.lexically_normal().string();
        autoStartRequested = true;
        if (!modeSpecified) {
            autoStartPlayerMode = true;
        }
    }

    if (oneShot) {
        std::error_code removeEc;
        if (!configFromAsset) {
            fs::remove(configPath, removeEc);
        }
    }
}

void Engine::applyAutoStartMode() {
    for (SceneObject& obj : sceneObjects) {
        if (!obj.hasAnimation) continue;
        obj.animation.runtimePlaying = false;
        obj.animation.runtimePaused = false;
        obj.animation.runtimeTime = 0.0f;
        obj.animation.runtimeDirection = 1.0f;
        obj.animation.runtimeInitialized = false;
        obj.animation.runtimeClipPath.clear();
    }
    playerMode = true;
    isPlaying = true;
    specMode = false;
    testMode = false;
    clearSelection();
    // Not captured yet: updateRuntimeMouseCapture grabs it once something steers with the
    // mouse, so a game that never does (menus, 2D) keeps its pointer from the start.
    gameViewCursorLocked = false;
    runtimeMouseCaptureArmed = true;
    gameViewportFocused = true;
    showHierarchy = false;
    showInspector = false;
    showFileBrowser = false;
    showConsole = false;
    showProjectBrowser = false;
    showRegistryPackagesWindow = false;
    showMeshBuilder = false;
    showEnvironmentWindow = false;
    showCameraWindow = false;
    showAnimationWindow = false;
    showAIPathfindingWindow = false;
    showPixelSpriteEditorWindow = false;
    showViewOutput = false;
    showSceneGizmos = false;
    showGameViewport = false;
    showBuildSettings = false;
    viewportFullscreen = true;
    if (editorWindow) {
        glfwFocusWindow(editorWindow);
    }
    updateHierarchyWorldTransforms();
    physics->onPlayStart(sceneObjects);
    audio.setPrefer2DSpatialAudio(isProject2DPipeline() || uiWorldMode);
    audio.onPlayStart(sceneObjects);
    startupSplashStartTime = glfwGetTime();
    applyBuildWindowTitle();
}

fs::path Engine::resolveSplashImagePath() const {
    if (buildSettings.splashImagePath.empty()) {
        return {};
    }
    fs::path path = fs::path(buildSettings.splashImagePath);
    if (path.is_absolute()) {
        return path;
    }
    if (projectManager.currentProject.isLoaded) {
        return projectManager.currentProject.projectPath / path;
    }
    return fs::current_path() / path;
}

void Engine::applyBuildWindowTitle() {
    if (!editorWindow) return;

    std::string displayName = buildSettings.buildName;
    if (displayName.empty()) {
        displayName = projectManager.currentProject.isLoaded
            ? projectManager.currentProject.name
            : "Modularity";
    }
    std::string version = buildSettings.version;
    std::string title;
    if (playerMode) {
        title = displayName;
        if (!version.empty()) {
            title += " " + version;
        }
    } else {
        title = "Modularity";
        if (!displayName.empty()) {
            title += " - " + displayName;
        }
        if (!version.empty()) {
            title += " (v" + version + ")";
        }
    }
    glfwSetWindowTitle(editorWindow, title.c_str());
}

void Engine::resetBuildSettings() {
    buildSettings.profileName = "Default";
    buildSettings.activeProfilePath = "BuildProfiles/Default.modubuild";
#ifdef _WIN32
    buildSettings.platform = BuildPlatform::Windows;
#else
    buildSettings.platform = BuildPlatform::Linux;
#endif
    buildSettings.architecture = "x86_64";
    buildSettings.configuration = "Release";
    buildSettings.includeEditor = false;
    buildSettings.runtimeOnly = true;
    buildSettings.leanRuntimeExport = false;
    buildSettings.shipScriptSdk = false;
    buildSettings.compressAudio = true;
    buildSettings.audioVorbisQuality = 5.0f;
    buildSettings.outputFolder = "Builds";
    buildSettings.outputName = "{buildName}-{version}-{platform}";
    buildSettings.moduleAssimp = "auto";
    buildSettings.modulePhysX = "auto";
    buildSettings.moduleJolt = "auto";
    buildSettings.moduleVulkan = "auto";
    buildSettings.moduleSndfile = "auto";
    buildSettings.moduleOpusfile = "auto";
    buildSettings.moduleMono = "auto";
    buildSettings.moduleOpenGLES = "auto";
    buildSettings.companyName = "DefaultCompany";
    buildSettings.buildName = projectManager.currentProject.name.empty()
        ? "MyProject"
        : projectManager.currentProject.name;
    buildSettings.version = "0.1.0";
    buildSettings.splashImagePath.clear();
    buildSettings.splashEnabled = false;
    buildSettings.splashDurationSeconds = 2.5f;
    buildSettings.packageStandaloneArchive = true;
    buildSettings.packagingMode = "extract";
    buildSettings.bundleVerifyIntegrity = true;
    buildSettings.gles2Shaders = false;
    buildSettings.preloadShaders = false;
    buildSettings.obfuscateScripts = false;
    buildSettings.obfuscationToolCommand.clear();
    buildSettings.antiTamper = false;
    buildSettings.antiTamperToolCommand.clear();
    buildSettings.developmentBuild = false;
    buildSettings.autoConnectProfiler = false;
    buildSettings.scriptDebugging = false;
    buildSettings.deepProfiling = false;
    buildSettings.scriptsOnlyBuild = false;
    buildSettings.serverBuild = false;
    buildSettings.compressionMethod = "Default";
    buildSettings.rendererAmbientColor = glm::vec3(0.2f, 0.2f, 0.2f);
    buildSettings.rendererShadowResolution = 512;
    buildSettings.rendererAutoReloadShaders = true;
    buildSettings.light2DLightingResolution = 0.75f;
    buildSettings.light2DLightingResolutionAuto = true;
    buildSettings.light2DLightingPixelFilter = false;
    buildSettings.light2DLightingSDR = false;
    buildSettings.editorCameraFov = FOV;
    buildSettings.editorCameraNear = NEAR_PLANE;
    buildSettings.editorCameraFar = FAR_PLANE;
    buildSettings.scenes.clear();
    buildSettingsSelectedIndex = -1;
    buildSettingsDirty = false;
}

bool Engine::addSceneToBuildSettings(const std::string& sceneName, bool enabled) {
    if (sceneName.empty()) return false;
    for (const auto& entry : buildSettings.scenes) {
        if (entry.name == sceneName) return false;
    }
    buildSettings.scenes.push_back({sceneName, enabled});
    buildSettingsDirty = true;
    return true;
}

void Engine::preloadRuntimeShaders() {
    if (!rendererInitialized || usingVulkan()) return;
    // The list itself is the switch: the export only writes it when preloading is on. The
    // staged build.modu is not reliable for this, since a project whose mirror only names
    // its active profile ships without that profile.
    const char* kListPath = "Resources/Shaders/preload.list";
    auto& assets = Modularity::Platform::GetAssetSource();
    if (!assets.Exists(kListPath)) return;
    const std::vector<uint8_t> bytes = assets.ReadAll(kListPath);
    const std::string text(bytes.begin(), bytes.end());

    const double start = glfwGetTime();
    int compiled = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t bar = line.find('|');
        if (bar == std::string::npos) continue;
        // Same strings the scene objects carry, so the draw finds this exact cache entry.
        // getShader falls back to the default program on a compile error and reports it.
        renderer.getShader(line.substr(0, bar), line.substr(bar + 1));
        ++compiled;
    }
    const double ms = (glfwGetTime() - start) * 1000.0;
    char message[128];
    std::snprintf(message, sizeof(message), "Precompiled %d shader program(s) at boot in %.1f ms",
                  compiled, ms);
    addConsoleMessage(message, ConsoleMessageType::Info);
    std::fprintf(stderr, "[Shaders] %s\n", message);
}

void Engine::loadBuildSettings() {
    std::fprintf(stderr, "[BuildSettings] loadBuildSettings() projectPath='%s'\n",
                 projectManager.currentProject.projectPath.string().c_str());

    resetBuildSettings();
    gameViewportResolutionIndex = 0;
    gameViewportCustomWidth = 1920;
    gameViewportCustomHeight = 1080;
    if (!projectManager.currentProject.isLoaded) return;

    fs::path buildPath = projectManager.currentProject.projectPath / "build.modu";
    std::fprintf(stderr, "[BuildSettings] checking '%s' exists=%d\n",
                 buildPath.string().c_str(), (int)fs::exists(buildPath));
    if (!fs::exists(buildPath)) {
        if (!projectManager.currentProject.currentSceneName.empty()) {
            addSceneToBuildSettings(projectManager.currentProject.currentSceneName, true);
        }
        saveBuildSettings();
        return;
    }

    auto trim = [](std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        auto end = s.find_last_not_of(" \t\r\n");
        if (start == std::string::npos || end == std::string::npos) {
            s.clear();
            return;
        }
        s = s.substr(start, end - start + 1);
    };

    std::string activeProfileToken;
    {
        std::ifstream mirrorFile(buildPath);
        std::string mirrorLine;
        while (std::getline(mirrorFile, mirrorLine)) {
            trim(mirrorLine);
            if (mirrorLine.rfind("activeProfile=", 0) == 0) {
                activeProfileToken = mirrorLine.substr(14);
                trim(activeProfileToken);
                break;
            }
        }
    }
    if (!activeProfileToken.empty()) {
        fs::path profilePath(activeProfileToken);
        if (profilePath.is_relative()) profilePath = projectManager.currentProject.projectPath / profilePath;
        if (fs::exists(profilePath)) {
            buildPath = profilePath;
            std::error_code relEc;
            fs::path rel = fs::relative(profilePath, projectManager.currentProject.projectPath, relEc);
            buildSettings.activeProfilePath = relEc ? activeProfileToken : rel.generic_string();
        }
    }

    std::ifstream file(buildPath);
    std::string line;
    while (std::getline(file, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("profileName=", 0) == 0) {
            buildSettings.profileName = line.substr(12);
            trim(buildSettings.profileName);
        } else if (line.rfind("profileId=", 0) == 0) {
        } else if (line.rfind("activeProfile=", 0) == 0) {
            buildSettings.activeProfilePath = line.substr(14);
            trim(buildSettings.activeProfilePath);
        } else if (line.rfind("platform=", 0) == 0) {
            std::string value = line.substr(9);
            trim(value);
            buildSettings.platform = buildPlatformFromSerializedName(value, buildSettings.platform);
        } else if (line.rfind("target.platform=", 0) == 0) {
            std::string value = line.substr(16);
            trim(value);
            buildSettings.platform = buildPlatformFromSerializedName(value, buildSettings.platform);
        } else if (line.rfind("architecture=", 0) == 0) {
            buildSettings.architecture = line.substr(13);
            trim(buildSettings.architecture);
        } else if (line.rfind("target.architecture=", 0) == 0) {
            buildSettings.architecture = line.substr(20);
            trim(buildSettings.architecture);
        } else if (line.rfind("configuration=", 0) == 0) {
            buildSettings.configuration = line.substr(14);
            trim(buildSettings.configuration);
        } else if (line.rfind("includeEditor=", 0) == 0) {
            buildSettings.includeEditor = line.substr(14) == "1";
        } else if (line.rfind("runtimeOnly=", 0) == 0) {
            buildSettings.runtimeOnly = line.substr(12) == "1";
        } else if (line.rfind("leanRuntimeExport=", 0) == 0) {
            buildSettings.leanRuntimeExport = line.substr(18) == "1";
        } else if (line.rfind("shipScriptSdk=", 0) == 0) {
            buildSettings.shipScriptSdk = line.substr(14) == "1";
        } else if (line.rfind("compressAudio=", 0) == 0) {
            buildSettings.compressAudio = line.substr(14) == "1";
        } else if (line.rfind("audioVorbisQuality=", 0) == 0) {
            try {
                buildSettings.audioVorbisQuality = std::clamp(std::stof(line.substr(19)), 0.0f, 10.0f);
            } catch (...) {
            }
        } else if (line.rfind("output.folder=", 0) == 0) {
            buildSettings.outputFolder = line.substr(14);
            trim(buildSettings.outputFolder);
        } else if (line.rfind("output.name=", 0) == 0) {
            buildSettings.outputName = line.substr(12);
            trim(buildSettings.outputName);
        } else if (line.rfind("module=", 0) == 0) {
            std::string value = line.substr(7);
            trim(value);
            size_t comma = value.find(',');
            if (comma != std::string::npos) {
                std::string name = value.substr(0, comma);
                std::string state = value.substr(comma + 1);
                trim(name);
                trim(state);
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(state.begin(), state.end(), state.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name == "assimp") buildSettings.moduleAssimp = state;
                else if (name == "physx") buildSettings.modulePhysX = state;
                else if (name == "jolt") buildSettings.moduleJolt = state;
                else if (name == "vulkan") buildSettings.moduleVulkan = state;
                else if (name == "sndfile") buildSettings.moduleSndfile = state;
                else if (name == "opusfile") buildSettings.moduleOpusfile = state;
                else if (name == "mono") buildSettings.moduleMono = state;
                else if (name == "opengl_es") buildSettings.moduleOpenGLES = state;
            }
        } else if (line.rfind("companyName=", 0) == 0) {
            buildSettings.companyName = line.substr(12);
            trim(buildSettings.companyName);
        } else if (line.rfind("buildName=", 0) == 0) {
            buildSettings.buildName = line.substr(10);
            trim(buildSettings.buildName);
        } else if (line.rfind("version=", 0) == 0) {
            buildSettings.version = line.substr(8);
            trim(buildSettings.version);
        } else if (line.rfind("splashImage=", 0) == 0) {
            buildSettings.splashImagePath = line.substr(12);
            trim(buildSettings.splashImagePath);
        } else if (line.rfind("splashEnabled=", 0) == 0) {
            buildSettings.splashEnabled = line.substr(14) == "1";
        } else if (line.rfind("splashDuration=", 0) == 0) {
            buildSettings.splashDurationSeconds = std::max(0.0f, std::stof(line.substr(15)));
        } else if (line.rfind("packageStandaloneArchive=", 0) == 0) {
            buildSettings.packageStandaloneArchive = line.substr(25) == "1";
        } else if (line.rfind("developmentBuild=", 0) == 0) {
            buildSettings.developmentBuild = line.substr(17) == "1";
        } else if (line.rfind("autoConnectProfiler=", 0) == 0) {
            buildSettings.autoConnectProfiler = line.substr(20) == "1";
        } else if (line.rfind("scriptDebugging=", 0) == 0) {
            buildSettings.scriptDebugging = line.substr(16) == "1";
        } else if (line.rfind("deepProfiling=", 0) == 0) {
            buildSettings.deepProfiling = line.substr(14) == "1";
        } else if (line.rfind("scriptsOnlyBuild=", 0) == 0) {
            buildSettings.scriptsOnlyBuild = line.substr(17) == "1";
        } else if (line.rfind("serverBuild=", 0) == 0) {
            buildSettings.serverBuild = line.substr(12) == "1";
        } else if (line.rfind("compressionMethod=", 0) == 0) {
            buildSettings.compressionMethod = line.substr(18);
            trim(buildSettings.compressionMethod);
        } else if (line.rfind("package.compression=", 0) == 0) {
            buildSettings.compressionMethod = line.substr(20);
            trim(buildSettings.compressionMethod);
        } else if (line.rfind("package.enabled=", 0) == 0) {
            buildSettings.packageStandaloneArchive = line.substr(16) == "1";
        } else if (line.rfind("packagingMode=", 0) == 0) {
            buildSettings.packagingMode = line.substr(14);
            trim(buildSettings.packagingMode);
            if (buildSettings.packagingMode != "loose" && buildSettings.packagingMode != "mount") {
                buildSettings.packagingMode = "extract";
            }
        } else if (line.rfind("bundleVerifyIntegrity=", 0) == 0) {
            buildSettings.bundleVerifyIntegrity = line.substr(22) == "1";
        } else if (line.rfind("gles2Shaders=", 0) == 0) {
            buildSettings.gles2Shaders = line.substr(13) == "1";
        } else if (line.rfind("preloadShaders=", 0) == 0) {
            buildSettings.preloadShaders = line.substr(15) == "1";
        } else if (line.rfind("obfuscateScripts=", 0) == 0) {
            buildSettings.obfuscateScripts = line.substr(17) == "1";
        } else if (line.rfind("obfuscationToolCommand=", 0) == 0) {
            buildSettings.obfuscationToolCommand = line.substr(23);
            trim(buildSettings.obfuscationToolCommand);
        } else if (line.rfind("antiTamper=", 0) == 0) {
            buildSettings.antiTamper = line.substr(11) == "1";
        } else if (line.rfind("antiTamperToolCommand=", 0) == 0) {
            buildSettings.antiTamperToolCommand = line.substr(22);
            trim(buildSettings.antiTamperToolCommand);
        } else if (line.rfind("rendererAmbient=", 0) == 0) {
            std::string value = line.substr(16);
            trim(value);
            std::replace(value.begin(), value.end(), ',', ' ');
            std::stringstream ss(value);
            ss >> buildSettings.rendererAmbientColor.x
               >> buildSettings.rendererAmbientColor.y
               >> buildSettings.rendererAmbientColor.z;
        } else if (line.rfind("rendererShadowResolution=", 0) == 0) {
            buildSettings.rendererShadowResolution = std::clamp(std::atoi(line.substr(25).c_str()), 128, 4096);
        } else if (line.rfind("rendererAutoReloadShaders=", 0) == 0) {
            buildSettings.rendererAutoReloadShaders = line.substr(26) == "1";
        } else if (line.rfind("light2DLightingResolutionAuto=", 0) == 0) {
            buildSettings.light2DLightingResolutionAuto = line.substr(30) == "1";
        } else if (line.rfind("light2DLightingResolution=", 0) == 0) {
            buildSettings.light2DLightingResolution = std::clamp(static_cast<float>(std::atof(line.substr(26).c_str())), 0.25f, 1.0f);
        } else if (line.rfind("light2DLightingPixelFilter=", 0) == 0) {
            buildSettings.light2DLightingPixelFilter = line.substr(27) == "1";
        } else if (line.rfind("light2DLightingSDR=", 0) == 0) {
            buildSettings.light2DLightingSDR = line.substr(19) == "1";
        } else if (line.rfind("editorCameraFov=", 0) == 0) {
            buildSettings.editorCameraFov = std::clamp(std::stof(line.substr(16)), 20.0f, 140.0f);
        } else if (line.rfind("editorCameraNear=", 0) == 0) {
            buildSettings.editorCameraNear = std::max(0.01f, std::stof(line.substr(17)));
        } else if (line.rfind("editorCameraFar=", 0) == 0) {
            buildSettings.editorCameraFar = std::max(buildSettings.editorCameraNear + 1.0f, std::stof(line.substr(16)));
        } else if (line.rfind("gameViewportResolutionIndex=", 0) == 0) {
            gameViewportResolutionIndex = std::clamp(std::atoi(line.substr(28).c_str()), 0, 5);
            std::fprintf(stderr, "[BuildSettings] loaded gameViewportResolutionIndex=%d\n",
                         gameViewportResolutionIndex);
        } else if (line.rfind("gameViewportCustomWidth=", 0) == 0) {
            gameViewportCustomWidth = std::clamp(std::atoi(line.substr(24).c_str()), 64, 8192);
        } else if (line.rfind("gameViewportCustomHeight=", 0) == 0) {
            gameViewportCustomHeight = std::clamp(std::atoi(line.substr(25).c_str()), 64, 8192);
        } else if (line.rfind("scene=", 0) == 0) {
            std::string value = line.substr(6);
            trim(value);
            size_t comma = value.find(',');
            if (comma != std::string::npos) {
                std::string name = value.substr(0, comma);
                std::string enabledStr = value.substr(comma + 1);
                trim(name);
                trim(enabledStr);
                if (!name.empty()) {
                    buildSettings.scenes.push_back({name, enabledStr == "1"});
                }
            }
        }
    }

    if (buildSettings.scenes.empty() && !projectManager.currentProject.currentSceneName.empty()) {
        addSceneToBuildSettings(projectManager.currentProject.currentSceneName, true);
    }
    if (buildSettings.buildName.empty()) {
        buildSettings.buildName = projectManager.currentProject.name.empty()
            ? "MyProject"
            : projectManager.currentProject.name;
    }
    if (buildSettings.companyName.empty()) {
        buildSettings.companyName = "DefaultCompany";
    }
    if (buildSettings.version.empty()) {
        buildSettings.version = "0.1.0";
    }
    if (buildSettings.profileName.empty()) {
        buildSettings.profileName = "Default";
    }
    if (buildSettings.activeProfilePath.empty()) {
        buildSettings.activeProfilePath = "BuildProfiles/" + buildSettings.profileName + ".modubuild";
    }
    if (buildSettings.configuration != "Debug") {
        buildSettings.configuration = "Release";
    }
    auto normalizeModuleState = [](std::string& state) {
        std::transform(state.begin(), state.end(), state.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (state != "on" && state != "off" && state != "auto") state = "auto";
    };
    normalizeModuleState(buildSettings.moduleAssimp);
    normalizeModuleState(buildSettings.modulePhysX);
    normalizeModuleState(buildSettings.moduleJolt);
    normalizeModuleState(buildSettings.moduleVulkan);
    normalizeModuleState(buildSettings.moduleSndfile);
    normalizeModuleState(buildSettings.moduleOpusfile);
    normalizeModuleState(buildSettings.moduleMono);
    normalizeModuleState(buildSettings.moduleOpenGLES);
    if (buildSettings.editorCameraFar <= buildSettings.editorCameraNear + 0.01f) {
        buildSettings.editorCameraFar = buildSettings.editorCameraNear + 0.01f;
    }
    buildSettings.splashDurationSeconds = std::clamp(buildSettings.splashDurationSeconds, 0.0f, 30.0f);
    if (rendererInitialized) {
        renderer.setAmbientColor(buildSettings.rendererAmbientColor);
        renderer.setShadowMapResolution(buildSettings.rendererShadowResolution);
        renderer.setShaderAutoReload(buildSettings.rendererAutoReloadShaders);
    }
    if (activeProfileToken.empty()) {
        saveBuildSettings();
    }
    applyBuildWindowTitle();
    buildSettingsDirty = false;
}

void Engine::saveBuildSettings() {
    if (!projectManager.currentProject.isLoaded) return;
    if (buildSettings.profileName.empty()) buildSettings.profileName = "Default";
    std::string safeProfileName = buildSettings.profileName;
    for (char& c : safeProfileName) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    }
    if (safeProfileName.empty()) safeProfileName = "Default";
    buildSettings.profileName = safeProfileName;
    buildSettings.activeProfilePath = "BuildProfiles/" + safeProfileName + ".modubuild";

    fs::path profilesDir = projectManager.currentProject.projectPath / "BuildProfiles";
    std::error_code ec;
    fs::create_directories(profilesDir, ec);
    fs::path profilePath = projectManager.currentProject.projectPath / buildSettings.activeProfilePath;
    fs::path buildPath = projectManager.currentProject.projectPath / "build.modu";

    auto writeSettings = [&](const fs::path& path, bool mirror) {
        std::ofstream file(path);
        file << (mirror ? "# build.modu\n" : "# Modularity build profile\n");
        file << "schemaVersion=1\n";
        file << "profileName=" << buildSettings.profileName << "\n";
        file << "profileId=" << buildSettings.profileName << "\n";
        if (mirror) file << "activeProfile=" << buildSettings.activeProfilePath << "\n";
        file << "platform=" << buildPlatformSerializedName(buildSettings.platform) << "\n";
        file << "target.platform=" << buildPlatformSerializedName(buildSettings.platform) << "\n";
        file << "architecture=" << buildSettings.architecture << "\n";
        file << "target.architecture=" << buildSettings.architecture << "\n";
        file << "configuration=" << buildSettings.configuration << "\n";
        file << "includeEditor=" << (buildSettings.includeEditor ? "1" : "0") << "\n";
        file << "runtimeOnly=" << (buildSettings.runtimeOnly ? "1" : "0") << "\n";
        file << "leanRuntimeExport=" << (buildSettings.leanRuntimeExport ? "1" : "0") << "\n";
        file << "shipScriptSdk=" << (buildSettings.shipScriptSdk ? "1" : "0") << "\n";
        file << "compressAudio=" << (buildSettings.compressAudio ? "1" : "0") << "\n";
        file << "audioVorbisQuality=" << buildSettings.audioVorbisQuality << "\n";
        file << "output.folder=" << buildSettings.outputFolder << "\n";
        file << "output.name=" << buildSettings.outputName << "\n";
        file << "module=assimp," << buildSettings.moduleAssimp << "\n";
        file << "module=physx," << buildSettings.modulePhysX << "\n";
        file << "module=jolt," << buildSettings.moduleJolt << "\n";
        file << "module=vulkan," << buildSettings.moduleVulkan << "\n";
        file << "module=sndfile," << buildSettings.moduleSndfile << "\n";
        file << "module=opusfile," << buildSettings.moduleOpusfile << "\n";
        file << "module=mono," << buildSettings.moduleMono << "\n";
        file << "module=opengl_es," << buildSettings.moduleOpenGLES << "\n";
        file << "companyName=" << buildSettings.companyName << "\n";
        file << "buildName=" << buildSettings.buildName << "\n";
        file << "version=" << buildSettings.version << "\n";
        file << "splashImage=" << buildSettings.splashImagePath << "\n";
        file << "splashEnabled=" << (buildSettings.splashEnabled ? "1" : "0") << "\n";
        file << "splashDuration=" << buildSettings.splashDurationSeconds << "\n";
        file << "packageStandaloneArchive=" << (buildSettings.packageStandaloneArchive ? "1" : "0") << "\n";
        file << "package.enabled=" << (buildSettings.packageStandaloneArchive ? "1" : "0") << "\n";
        file << "package.compression=" << buildSettings.compressionMethod << "\n";
        file << "packagingMode=" << buildSettings.packagingMode << "\n";
        file << "bundleVerifyIntegrity=" << (buildSettings.bundleVerifyIntegrity ? "1" : "0") << "\n";
        file << "gles2Shaders=" << (buildSettings.gles2Shaders ? "1" : "0") << "\n";
        file << "preloadShaders=" << (buildSettings.preloadShaders ? "1" : "0") << "\n";
        file << "obfuscateScripts=" << (buildSettings.obfuscateScripts ? "1" : "0") << "\n";
        file << "obfuscationToolCommand=" << buildSettings.obfuscationToolCommand << "\n";
        file << "antiTamper=" << (buildSettings.antiTamper ? "1" : "0") << "\n";
        file << "antiTamperToolCommand=" << buildSettings.antiTamperToolCommand << "\n";
        file << "developmentBuild=" << (buildSettings.developmentBuild ? "1" : "0") << "\n";
        file << "autoConnectProfiler=" << (buildSettings.autoConnectProfiler ? "1" : "0") << "\n";
        file << "scriptDebugging=" << (buildSettings.scriptDebugging ? "1" : "0") << "\n";
        file << "deepProfiling=" << (buildSettings.deepProfiling ? "1" : "0") << "\n";
        file << "scriptsOnlyBuild=" << (buildSettings.scriptsOnlyBuild ? "1" : "0") << "\n";
        file << "serverBuild=" << (buildSettings.serverBuild ? "1" : "0") << "\n";
        file << "compressionMethod=" << buildSettings.compressionMethod << "\n";
        file << "rendererAmbient="
             << buildSettings.rendererAmbientColor.x << ","
             << buildSettings.rendererAmbientColor.y << ","
             << buildSettings.rendererAmbientColor.z << "\n";
        file << "rendererShadowResolution=" << buildSettings.rendererShadowResolution << "\n";
        file << "rendererAutoReloadShaders=" << (buildSettings.rendererAutoReloadShaders ? "1" : "0") << "\n";
        file << "light2DLightingResolution=" << buildSettings.light2DLightingResolution << "\n";
        file << "light2DLightingResolutionAuto=" << (buildSettings.light2DLightingResolutionAuto ? "1" : "0") << "\n";
        file << "light2DLightingPixelFilter=" << (buildSettings.light2DLightingPixelFilter ? "1" : "0") << "\n";
        file << "light2DLightingSDR=" << (buildSettings.light2DLightingSDR ? "1" : "0") << "\n";
        file << "editorCameraFov=" << buildSettings.editorCameraFov << "\n";
        file << "editorCameraNear=" << buildSettings.editorCameraNear << "\n";
        file << "editorCameraFar=" << buildSettings.editorCameraFar << "\n";
        file << "gameViewportResolutionIndex=" << std::clamp(gameViewportResolutionIndex, 0, 5) << "\n";
        file << "gameViewportCustomWidth=" << std::clamp(gameViewportCustomWidth, 64, 8192) << "\n";
        file << "gameViewportCustomHeight=" << std::clamp(gameViewportCustomHeight, 64, 8192) << "\n";
        for (const auto& scene : buildSettings.scenes) {
            file << "scene=" << scene.name << "," << (scene.enabled ? "1" : "0") << "\n";
        }
    };
    writeSettings(profilePath, false);
    writeSettings(buildPath, true);
    applyBuildWindowTitle();
    buildSettingsDirty = false;
}

void Engine::selectBuildSettingsProfile(const fs::path& relativeProfilePath) {
    if (!projectManager.currentProject.isLoaded || relativeProfilePath.empty()) return;
    fs::path mirrorPath = projectManager.currentProject.projectPath / "build.modu";
    std::ofstream mirror(mirrorPath, std::ios::trunc);
    if (!mirror.is_open()) {
        addConsoleMessage("Failed to select build profile: could not write build.modu", ConsoleMessageType::Error);
        return;
    }
    mirror << "# build.modu\n";
    mirror << "activeProfile=" << relativeProfilePath.generic_string() << "\n";
    mirror.close();
    loadBuildSettings();
}

void Engine::saveBuildSettingsProfileAs(const std::string& profileName) {
    buildSettings.profileName = profileName.empty() ? "Default" : profileName;
    saveBuildSettings();
}

void Engine::duplicateBuildSettingsProfile(const std::string& profileName) {
    saveBuildSettingsProfileAs(profileName);
}

bool Engine::deleteBuildSettingsProfile(const std::string& profileName) {
    if (!projectManager.currentProject.isLoaded || profileName.empty() || profileName == "Default") {
        return false;
    }
    std::string safeName = profileName;
    for (char& c : safeName) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    }
    std::error_code ec;
    fs::remove(projectManager.currentProject.projectPath / "BuildProfiles" / (safeName + ".modubuild"), ec);
    buildSettings.profileName = "Default";
    buildSettings.activeProfilePath = "BuildProfiles/Default.modubuild";
    saveBuildSettings();
    return !ec;
}

Engine::BuildModuleProjectScan Engine::scanProjectForBuildModules() const {
    BuildModuleProjectScan scan;
    const auto& project = projectManager.currentProject;
    if (!project.isLoaded) return scan;
    scan.valid = true;
    scan.projectRoot = project.projectPath;

    auto readText = [](const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        std::ostringstream text;
        text << in.rdbuf();
        out = text.str();
        return true;
    };

    // Build scenes: meshes they place and the C# scripts they run.
    std::vector<std::string> sceneNames;
    for (const auto& scene : buildSettings.scenes) {
        if (scene.enabled && !scene.name.empty()) sceneNames.push_back(scene.name);
    }
    if (!project.currentSceneName.empty() &&
        std::find(sceneNames.begin(), sceneNames.end(), project.currentSceneName) == sceneNames.end()) {
        sceneNames.push_back(project.currentSceneName);
    }
    for (const std::string& sceneName : sceneNames) {
        std::string text;
        if (!readText(project.scenesPath / (sceneName + ".scene"), text)) continue;
        if (scan.modelAsset.empty()) {
            std::set<std::string> importers;
            scanTextForAndroidModelImporters(text, importers);
            if (!importers.empty()) scan.modelAsset = "a mesh in scene " + sceneName;
        }
        if (scan.managedScript.empty()) {
            std::string script;
            if (textReferencesManagedScript(text, script)) scan.managedScript = fs::path(script).filename().string();
        }
    }
    // The open scene may hold a C# script that has not been saved yet.
    for (const auto& obj : sceneObjects) {
        if (!scan.managedScript.empty()) break;
        for (const auto& sc : obj.scripts) {
            if (sc.language == ScriptLanguage::CSharp && !sc.path.empty()) {
                scan.managedScript = fs::path(sc.path).filename().string();
                break;
            }
        }
    }

    // Assets: scripts can load any model or clip by path at runtime, so every file counts, not
    // only the ones a scene names.
    std::error_code ec;
    const fs::path assetsRoot = project.projectPath / "Assets";
    if (fs::is_directory(assetsRoot, ec)) {
        for (auto it = fs::recursive_directory_iterator(assetsRoot, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || fileEc) continue;
            const fs::path& path = it->path();
            const std::string ext = lowerExtension(path);
            if (scan.modelAsset.empty()) {
                std::set<std::string> importers;
                if (appendAssimpImporterForExtension(ext, importers)) scan.modelAsset = path.filename().string();
            }
            if (scan.sndfileClip.empty() && isSndfileOnlyAudioExtension(ext)) {
                scan.sndfileClip = path.filename().string();
            }
            if (scan.opusClip.empty() &&
                (ext == ".opus" || ((ext == ".ogg" || ext == ".oga") && isOggOpusFile(path)))) {
                scan.opusClip = path.filename().string();
            }
            if (scan.managedScript.empty() && ext == ".moduobj") {
                std::string text;
                std::string script;
                if (readText(path, text) && textReferencesManagedScript(text, script)) {
                    scan.managedScript = fs::path(script).filename().string();
                }
            }
        }
    }
    return scan;
}

std::vector<Engine::BuildModuleDecision> Engine::resolveBuildModules(const BuildModuleProjectScan& scan) {
    const auto& project = projectManager.currentProject;
    const bool android = buildSettings.platform == BuildPlatform::Android;
    const bool usesPhysX = project.physicsSettings.backend == PhysicsBackendType::PhysX;
    const bool usesVulkan = project.rendererBackend == Modularity::GraphicsBackend::Vulkan;

    std::vector<BuildModuleDecision> decisions;
    decisions.reserve(8);
    auto add = [&](const char* label, const char* option, std::string& setting,
                   bool autoEnabled, std::string autoReason) {
        BuildModuleDecision decision;
        decision.label = label;
        decision.cmakeOption = option;
        decision.setting = &setting;
        std::string state = setting;
        std::transform(state.begin(), state.end(), state.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        decision.automatic = state != "on" && state != "off";
        decision.autoEnabled = autoEnabled;
        decision.autoReason = autoReason;
        if (!decision.automatic) {
            decision.enabled = state == "on";
            decision.reason = decision.enabled ? "Forced on in this build profile."
                                               : "Forced off in this build profile.";
        } else {
            decision.enabled = autoEnabled;
            decision.reason = std::move(autoReason);
        }
        decisions.push_back(std::move(decision));
    };
    const std::string unscanned = "Project not scanned yet; kept on to be safe.";

    if (android) {
        add("Jolt", "MODULARITY_ENABLE_JOLT", buildSettings.moduleJolt, true,
            usesPhysX ? "PhysX has no Android build yet, so the player falls back to Jolt."
                      : "Project physics backend is Jolt.");
        add("PhysX", "MODULARITY_ENABLE_PHYSX", buildSettings.modulePhysX, false,
            "PhysX has no Android build yet.");
    } else {
        add("Jolt", "MODULARITY_ENABLE_JOLT", buildSettings.moduleJolt, !usesPhysX,
            usesPhysX ? "Project physics backend is PhysX, so Jolt is left out."
                      : "Project physics backend is Jolt.");
        add("PhysX", "MODULARITY_ENABLE_PHYSX", buildSettings.modulePhysX, usesPhysX,
            usesPhysX ? "Project physics backend is PhysX."
                      : "Project physics backend is Jolt, so PhysX is left out.");
    }

    if (!scan.valid) {
        add("Assimp", "MODULARITY_ENABLE_ASSIMP", buildSettings.moduleAssimp, true, unscanned);
    } else if (!scan.modelAsset.empty()) {
        add("Assimp", "MODULARITY_ENABLE_ASSIMP", buildSettings.moduleAssimp, true,
            "Loads model files at runtime (" + scan.modelAsset + ").");
    } else {
        add("Assimp", "MODULARITY_ENABLE_ASSIMP", buildSettings.moduleAssimp, false,
            "No model files in Assets or the build scenes.");
    }

    if (android) {
        add("Vulkan", "MODULARITY_ENABLE_VULKAN", buildSettings.moduleVulkan, false,
            "Android renders with OpenGL ES.");
    } else {
        add("Vulkan", "MODULARITY_ENABLE_VULKAN", buildSettings.moduleVulkan, usesVulkan,
            usesVulkan ? "Project renderer is Vulkan." : "Project renders with OpenGL.");
    }

    if (android) {
        add("sndfile", "MODULARITY_ENABLE_SNDFILE", buildSettings.moduleSndfile, false,
            "libsndfile is not built for Android yet.");
        add("opusfile", "MODULARITY_ENABLE_OPUSFILE", buildSettings.moduleOpusfile, false,
            "libopusfile is not built for Android yet.");
        add("Mono", "MODULARITY_USE_MONO", buildSettings.moduleMono, false,
            "C# scripts are not supported on Android.");
    } else if (!scan.valid) {
        add("sndfile", "MODULARITY_ENABLE_SNDFILE", buildSettings.moduleSndfile, true, unscanned);
        add("opusfile", "MODULARITY_ENABLE_OPUSFILE", buildSettings.moduleOpusfile, true, unscanned);
        add("Mono", "MODULARITY_USE_MONO", buildSettings.moduleMono, true, unscanned);
    } else {
        add("sndfile", "MODULARITY_ENABLE_SNDFILE", buildSettings.moduleSndfile, !scan.sndfileClip.empty(),
            scan.sndfileClip.empty()
                ? "Every clip is WAV, FLAC, MP3 or Vorbis, which the built-in decoder reads."
                : "Needed to decode " + scan.sndfileClip + ".");
        add("opusfile", "MODULARITY_ENABLE_OPUSFILE", buildSettings.moduleOpusfile, !scan.opusClip.empty(),
            scan.opusClip.empty() ? "No Opus clips in Assets."
                                  : "Needed to decode Opus clip " + scan.opusClip + ".");
        add("Mono", "MODULARITY_USE_MONO", buildSettings.moduleMono, !scan.managedScript.empty(),
            scan.managedScript.empty() ? "No build scene runs a C# script."
                                       : "A build scene runs C# script " + scan.managedScript + ".");
    }

    add("OpenGL ES", "MODULARITY_USE_OPENGL_ES", buildSettings.moduleOpenGLES, android,
        android ? "Android renders with OpenGL ES." : "Desktop renders with desktop OpenGL.");
    return decisions;
}

void Engine::startExportBuild(const fs::path& outputDir, bool runAfter) {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("No project loaded for export", ConsoleMessageType::Warning);
        return;
    }
    if (exportJob.active) return;

    if (projectManager.currentProject.hasUnsavedChanges) {
        if (!saveCurrentScene()) {
            return;
        }
    } else {
        projectManager.currentProject.saveProjectFile();
    }
    saveBuildSettings();
    // The export copies project.modu from disk; land any settings save still being coalesced.
    Modularity::FlushDurableWrites();

    std::error_code ec;
    fs::path normalizedOut = fs::absolute(outputDir, ec);
    if (ec) {
        addConsoleMessage("Export failed: invalid output path.", ConsoleMessageType::Error);
        return;
    }
    fs::create_directories(normalizedOut, ec);
    if (ec) {
        addConsoleMessage("Export failed: unable to create output folder.", ConsoleMessageType::Error);
        return;
    }

    fs::path sourceRoot;
    {
        std::vector<fs::path> sourceCandidates;
#ifdef MODULARITY_SOURCE_DIR
        sourceCandidates.emplace_back(MODULARITY_SOURCE_DIR);
#endif
        sourceCandidates.push_back(fs::current_path());
        fs::path exePath = resolveCurrentExecutablePath();
        if (!exePath.empty()) {
            sourceCandidates.push_back(exePath.parent_path());
            sourceCandidates.push_back(exePath.parent_path().parent_path());
        }
        for (const auto& candidate : sourceCandidates) {
            sourceRoot = findCMakeSourceRoot(candidate);
            if (!sourceRoot.empty()) {
                break;
            }
        }
    }
    if (sourceRoot.empty()) {
        addConsoleMessage("Export failed: could not locate CMakeLists.txt.", ConsoleMessageType::Error);
        return;
    }

    std::string startScene = projectManager.currentProject.currentSceneName;
    if (startScene.empty()) {
        for (const auto& scene : buildSettings.scenes) {
            if (scene.enabled) {
                startScene = scene.name;
                break;
            }
        }
    }

    std::string buildNameDisplay = buildSettings.buildName.empty()
        ? (projectManager.currentProject.name.empty() ? "Game" : projectManager.currentProject.name)
        : buildSettings.buildName;
    std::string buildVersionDisplay = buildSettings.version.empty() ? "0.1.0" : buildSettings.version;
    std::string executableStem = sanitizeBuildToken(buildNameDisplay, "Game");
    std::string safeVersion = sanitizeBuildToken(buildVersionDisplay, "0.1.0");
    std::string platformLabel = buildPlatformSerializedName(buildSettings.platform);
    std::string packageStem = executableStem + "-" + safeVersion + "-" + platformLabel;
    fs::path exportRoot = normalizedOut / packageStem;
    fs::path archivePath = normalizedOut / (packageStem + ".tar.gz");
    std::string executableFileName = executableStem;
#ifdef _WIN32
    executableFileName += ".exe";
#endif

    fs::path projectRoot = projectManager.currentProject.projectPath;
    fs::path scenesPath = projectManager.currentProject.scenesPath;
    fs::path scriptsPath = projectManager.currentProject.scriptsPath;
    fs::path scriptsConfigPath = resolveScriptsConfigPath(projectManager.currentProject);
    std::vector<std::string> runtimeSceneNames;
    {
        std::unordered_set<std::string> seenScenes;
        for (const auto& scene : buildSettings.scenes) {
            if (!scene.enabled || scene.name.empty()) continue;
            if (seenScenes.insert(scene.name).second) {
                runtimeSceneNames.push_back(scene.name);
            }
        }
        if (!startScene.empty() && seenScenes.insert(startScene).second) {
            runtimeSceneNames.push_back(startScene);
        }
    }
    std::string exportSplashImagePath = buildSettings.splashImagePath;
    std::vector<fs::path> assignedNativeScriptSources;
    {
        std::unordered_set<std::string> seenSources;
        for (const auto& obj : sceneObjects) {
            for (const auto& sc : obj.scripts) {
                if (sc.path.empty()) continue;
                if (sc.language != ScriptLanguage::C && sc.language != ScriptLanguage::Cpp) continue;
                fs::path sourcePath(sc.path);
                std::string key = sourcePath.lexically_normal().string();
                if (seenSources.insert(key).second) {
                    assignedNativeScriptSources.push_back(sourcePath);
                }
            }
        }
    }
    bool packageStandaloneArchive = buildSettings.packageStandaloneArchive;
    bool leanRuntimeExport = buildSettings.leanRuntimeExport;
    std::string packagingMode = buildSettings.packagingMode;   // "loose" / "extract" / "mount"
    bool bundleVerifyIntegrity = buildSettings.bundleVerifyIntegrity;
    bool gles2Shaders = buildSettings.gles2Shaders;
    bool preloadShaders = buildSettings.preloadShaders || buildSettings.gles2Shaders;
    bool obfuscateScripts = buildSettings.obfuscateScripts;
    std::string obfuscationToolCommand = buildSettings.obfuscationToolCommand;
    bool antiTamper = buildSettings.antiTamper;
    std::string antiTamperToolCommand = buildSettings.antiTamperToolCommand;
    bool compressAudio = buildSettings.compressAudio;
    float audioVorbisQuality = buildSettings.audioVorbisQuality;
    bool includeEditorInExport = buildSettings.includeEditor;
    bool shipScriptSdk = buildSettings.shipScriptSdk || buildSettings.developmentBuild;
    std::string buildConfiguration = buildSettings.configuration == "Debug" ? "Debug" : "Release";
    auto moduleState = [](std::string state) {
        std::transform(state.begin(), state.end(), state.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return (state == "on" || state == "off" || state == "auto") ? state : std::string("auto");
    };
    std::string moduleAssimp = moduleState(buildSettings.moduleAssimp);
    // Every module is passed explicitly, "auto" included. Leaving an auto module off the command
    // line meant CMake's cached value won: the shared player-cache tree kept a module OFF from an
    // earlier export even after the project started needing it.
    buildModuleScan = scanProjectForBuildModules();
    std::vector<std::pair<std::string, bool>> moduleFlags;
    std::string moduleSummary = "Engine modules:\n";
    for (const BuildModuleDecision& decision : resolveBuildModules(buildModuleScan)) {
        moduleFlags.emplace_back(decision.cmakeOption, decision.enabled);
        moduleSummary += std::string("  ") + decision.label + ": " + (decision.enabled ? "on" : "off") +
                         (decision.automatic ? " (auto) - " : " - ") + decision.reason + "\n";
    }
    auto appendCMakeModuleFlags = [moduleFlags](std::string& command, bool androidTarget) {
        for (const auto& flag : moduleFlags) {
            // The Android configure line sets Assimp itself, from the importers it detected.
            if (androidTarget && flag.first == "MODULARITY_ENABLE_ASSIMP") continue;
            command += " -D" + flag.first + (flag.second ? "=ON" : "=OFF");
        }
    };
    const bool androidExport = buildSettings.platform == BuildPlatform::Android;
    std::string androidAbi = buildSettings.architecture.empty()
        ? std::string(defaultArchitectureForPlatform(BuildPlatform::Android))
        : buildSettings.architecture;
    fs::path androidApkPath = normalizedOut / (packageStem + ".apk");
    std::string exportOutputName = androidExport ? androidApkPath.filename().string() : executableFileName;
    std::string androidPackageName = makeAndroidPackageName(buildSettings.companyName, buildNameDisplay);
    bool androidDebuggable = buildSettings.developmentBuild;
    std::vector<std::string> androidAssimpImporters;
    if (androidExport) {
        androidAssimpImporters = detectAndroidAssimpImporters(projectRoot, scenesPath, runtimeSceneNames);
        if (moduleAssimp == "off") {
            androidAssimpImporters.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(exportMutex);
        exportJob = ExportJobState{};
        exportJob.active = true;
        exportJob.runAfter = androidExport ? false : runAfter;
        exportJob.progress = 0.02f;
        exportJob.status = "Preparing export...";
        exportJob.log = moduleSummary;
        exportJob.outputDir = androidExport ? normalizedOut : exportRoot;
        exportJob.executableName = exportOutputName;
        exportJob.archivePath = androidExport
            ? androidApkPath
            : (packageStandaloneArchive ? archivePath : fs::path{});
    }
    exportCancelRequested = false;

    AndroidXrManifestOptions androidXrOptions;
    {
        const Modularity::XR::ProjectOpenXRSettings& xrSettings =
            projectManager.currentProject.openXRSettings;
        const Modularity::XR::XRTargetSettings& questTarget =
            xrSettings.target(Modularity::XR::XRPlatformTarget::MetaQuest);
        const Modularity::XR::XRTargetSettings& androidTarget =
            xrSettings.target(Modularity::XR::XRPlatformTarget::Android);
        androidXrOptions.enabled =
            xrSettings.enabled && (questTarget.enabled || androidTarget.enabled);
        androidXrOptions.questSupport = xrSettings.enabled && questTarget.enabled;
        androidXrOptions.handTracking =
            androidXrOptions.questSupport &&
            questTarget.hasFeature(
                Modularity::XR::FeatureSerializationId(Modularity::XR::XRFeature::HandTracking)) &&
            Modularity::XR::IsFeatureImplemented(Modularity::XR::XRFeature::HandTracking);
    }

    if (androidExport) {
        auto future = std::async(std::launch::async,
            [this, normalizedOut, exportRoot, sourceRoot, projectRoot, startScene,
             scenesPath, scriptsConfigPath, runtimeSceneNames, exportSplashImagePath,
             packageStem, androidAbi, androidApkPath, androidPackageName,
             buildNameDisplay, buildVersionDisplay, androidDebuggable,
             androidAssimpImporters, buildConfiguration, leanRuntimeExport,
             compressAudio, audioVorbisQuality, gles2Shaders, preloadShaders,
             appendCMakeModuleFlags, moduleAssimp, androidXrOptions]() {
            ExportJobResult result;
            result.outputDir = normalizedOut;
            result.executableName = androidApkPath.filename().string();
            result.archivePath = androidApkPath;

            auto setStatus = [this](float value, const std::string& status) {
                std::lock_guard<std::mutex> lock(exportMutex);
                exportJob.progress = value;
                exportJob.status = status;
            };
            auto appendLog = [this](const std::string& text) {
                std::lock_guard<std::mutex> lock(exportMutex);
                exportJob.log += text;
                if (!exportJob.log.empty() && exportJob.log.back() != '\n') {
                    exportJob.log += '\n';
                }
            };

            std::error_code ec;
            const fs::path ndkRoot = resolveAndroidNdkPath();
            if (ndkRoot.empty()) {
                result.message = "Android NDK not found. Set ANDROID_NDK_ROOT, ANDROID_NDK_HOME, or ANDROID_NDK.";
                return result;
            }
            const fs::path toolchainFile = ndkRoot / "build" / "cmake" / "android.toolchain.cmake";
            if (!fs::exists(toolchainFile, ec) || ec) {
                result.message = "Android NDK is missing android.toolchain.cmake: " + toolchainFile.string();
                return result;
            }

            const fs::path sdkRoot = findAndroidSdkRoot();
            if (sdkRoot.empty()) {
                result.message = "Android SDK not found. Set ANDROID_SDK_ROOT or ANDROID_HOME.";
                return result;
            }
            std::vector<fs::path> androidJarSdkRoots;
            androidJarSdkRoots.push_back(sdkRoot);
            for (const fs::path& localSdkRoot : {
                     ModularityAndroidDir(sourceRoot, "android-sdk"),
                     ModularityAndroidDir(sourceRoot, "android-sdk-smoke")
                 }) {
                if (localSdkRoot != sdkRoot) {
                    androidJarSdkRoots.push_back(localSdkRoot);
                }
            }
            fs::path androidJarSdkRoot;
            int targetApi = 0;
            const fs::path androidJar =
                findLatestAndroidJarInRoots(androidJarSdkRoots, androidJarSdkRoot, targetApi);
            if (androidJar.empty()) {
                const fs::path localSdkRoot = ModularityAndroidDir(sourceRoot, "android-sdk");
                result.message =
                    "Android SDK platform android.jar not found under: " + sdkRoot.string() +
                    "\nInstall one with: " +
                    quotePath(sdkRoot / "cmdline-tools" / "latest" / "bin" / androidToolName("sdkmanager")) +
                    " --sdk_root=" + quotePath(localSdkRoot) + " \"platforms;android-35\"";
                return result;
            }
            if (androidJarSdkRoot != sdkRoot) {
                appendLog("Using Android platform from fallback SDK root: " + androidJarSdkRoot.string());
            }
            AndroidBuildTools buildTools = findLatestAndroidBuildTools(sdkRoot);
            if (buildTools.aapt2.empty() || buildTools.zipalign.empty() || buildTools.apksigner.empty()) {
                result.message = "Android SDK build-tools missing aapt2, zipalign, or apksigner.";
                return result;
            }

            fs::remove_all(exportRoot, ec);
            ec.clear();
            fs::create_directories(exportRoot, ec);
            if (ec) {
                result.message = "Failed to create Android export directory.";
                return result;
            }
            fs::remove(androidApkPath, ec);

            const fs::path buildRoot = normalizedOut / ("_android_build_" + androidAbi);
            fs::create_directories(buildRoot, ec);
            if (ec) {
                result.message = "Failed to create Android build directory.";
                return result;
            }

            setStatus(0.08f, "Configuring Android build...");
            const bool androidEnableAssimp = moduleAssimp == "on" || !androidAssimpImporters.empty();
            if (androidEnableAssimp) {
                appendLog("Android model importers: " + joinStrings(androidAssimpImporters, ", "));
            } else {
                appendLog("No model assets detected for Android export; Assimp disabled.");
            }
            std::string generatorArgs;
            if (!androidCMakeGeneratorArgs(sdkRoot, buildRoot, generatorArgs, result.message)) return result;
            std::string configureCmd =
                "cmake" + generatorArgs + " -S " + quotePath(sourceRoot) +
                " -B " + quotePath(buildRoot) +
                " -DCMAKE_BUILD_TYPE=" + buildConfiguration +
                " -DMODULARITY_BUILD_EDITOR=OFF"
                " -DCMAKE_TOOLCHAIN_FILE=" + quotePath(toolchainFile) +
                " -DANDROID_ABI=" + androidAbi +
                " -DANDROID_PLATFORM=android-26"
                " -DANDROID_STL=c++_shared"
                " -DMODULARITY_ENABLE_ASSIMP=" + std::string(androidEnableAssimp ? "ON" : "OFF");
            appendCMakeModuleFlags(configureCmd, true);
            if (androidEnableAssimp) {
                configureCmd +=
                    " -DMODULARITY_ASSIMP_IMPORTERS=" +
                    quotePath(joinStrings(androidAssimpImporters, ";"));
            }
            int configureExit = 0;
            appendLog("Running: " + configureCmd);
            if (!runCommandStreaming(configureCmd + " 2>&1", appendLog, &configureExit)) {
                result.message = "Android CMake configure failed (exit code " + std::to_string(configureExit) + ").";
                return result;
            }

            if (exportCancelRequested.load()) {
                result.message = "Export cancelled.";
                return result;
            }

            setStatus(0.32f, "Building Android player library...");
            std::string buildCmd = "cmake --build " + quotePath(buildRoot) +
                                   " --config " + buildConfiguration + " --target core_player" + androidBuildParallelArgs();
            int buildExit = 0;
            appendLog("Running: " + buildCmd);
            auto onBuildChunk = [this, &appendLog](const std::string& chunk) {
                appendLog(chunk);
                size_t open = chunk.find('[');
                size_t pct = chunk.find('%');
                if (open != std::string::npos && pct != std::string::npos && pct > open) {
                    std::string num = chunk.substr(open + 1, pct - open - 1);
                    num.erase(0, num.find_first_not_of(" \t"));
                    num.erase(num.find_last_not_of(" \t") + 1);
                    int value = std::atoi(num.c_str());
                    if (value >= 0 && value <= 100) {
                        float progress = 0.32f + (value / 100.0f) * 0.28f;
                        std::lock_guard<std::mutex> lock(exportMutex);
                        exportJob.progress = progress;
                        exportJob.status = "Building Android player library (" + std::to_string(value) + "%)";
                    }
                }
            };
            if (!runCommandStreaming(buildCmd + " 2>&1", onBuildChunk, &buildExit)) {
                result.message = "Android player build failed (exit code " + std::to_string(buildExit) + ").";
                return result;
            }

            if (exportCancelRequested.load()) {
                result.message = "Export cancelled.";
                return result;
            }

            const fs::path playerSo = findAndroidSharedLibrary(buildRoot);
            if (playerSo.empty()) {
                result.message = "Built Android player library not found: libModularityPlayer.so";
                return result;
            }

            const fs::path apkStageRoot = exportRoot / "apk";
            const fs::path assetsRoot = apkStageRoot / "assets";
            const fs::path libDir = apkStageRoot / "lib" / androidAbi;
            if (!stageAndroidCxxRuntime(ndkRoot, androidAbi, libDir, result.message)) return result;
            fs::create_directories(assetsRoot, ec);
            fs::create_directories(libDir, ec);
            if (ec) {
                result.message = "Failed to create APK staging directories.";
                return result;
            }
            fs::copy_file(playerSo, libDir / "libModularityPlayer.so",
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                result.message = "Failed to stage Android player library.";
                return result;
            }
            const fs::path stagedPlayerSo = libDir / "libModularityPlayer.so";
            const fs::path stripTool = findAndroidLlvmStrip(ndkRoot);
            if (!stripTool.empty()) {
                int stripExit = 0;
                const std::string stripCmd =
                    quotePath(stripTool) + " --strip-unneeded " + quotePath(stagedPlayerSo);
                appendLog("Running: " + stripCmd);
                if (!runCommandStreaming(stripCmd + " 2>&1", appendLog, &stripExit)) {
                    appendLog("Warning: Android symbol stripping failed (exit code " +
                              std::to_string(stripExit) + "); APK will include unstripped native library.");
                }
            } else {
                appendLog("Warning: llvm-strip not found; APK will include unstripped native library.");
            }

            std::string copyError;
            const fs::path runtimeStageRoot = exportRoot / "_runtime_stage";
            fs::create_directories(runtimeStageRoot, ec);
            if (ec) {
                result.message = "Failed to create runtime staging directory.";
                return result;
            }
            RuntimeExportStager runtimeStager(projectRoot, runtimeStageRoot);

            setStatus(0.62f, "Staging Android runtime resources...");
            if (!CopyRuntimeResourcesSubset(sourceRoot, runtimeStageRoot, leanRuntimeExport, copyError)) {
                result.message = copyError;
                return result;
            }
            appendLog("Skipping desktop package cache copy for Android; native code is staged into apk/lib/" + androidAbi + ".");
            if (!CopyFileIntoRuntimeRoot(projectRoot / "project.modu", runtimeStageRoot, "project.modu", copyError)) {
                result.message = copyError;
                return result;
            }

            RuntimeShaderPairs runtimeShaderPairs;
            if (!runtimeSceneNames.empty()) {
                const float sceneCount = static_cast<float>(runtimeSceneNames.size());
                for (size_t i = 0; i < runtimeSceneNames.size(); ++i) {
                    if (exportCancelRequested.load()) {
                        result.message = "Export cancelled.";
                        return result;
                    }
                    const std::string& sceneName = runtimeSceneNames[i];
                    const fs::path sourceScenePath = scenesPath / (sceneName + ".scene");
                    const float progress = 0.66f + 0.06f * static_cast<float>(i) / std::max(1.0f, sceneCount);
                    setStatus(progress, "Serializing runtime scene: " + sceneName + "...");
                    if (!StageRuntimeScene(sourceScenePath, sceneName, runtimeStager, runtimeStageRoot, copyError,
                                           &runtimeShaderPairs)) {
                        result.message = copyError;
                        return result;
                    }
                }
            }
            if (!StageRuntimeShaderPrograms(runtimeStageRoot, runtimeShaderPairs, gles2Shaders, preloadShaders,
                                            appendLog, copyError)) {
                result.message = copyError;
                return result;
            }

            std::string stagedSplashPath = exportSplashImagePath;
            if (!stagedSplashPath.empty()) {
                runtimeStager.setReferenceOwner("Build settings");
                runtimeStager.setReferenceLabel("splash image");
                if (!runtimeStager.stageFileReference(stagedSplashPath, "Splash", copyError)) {
                    result.message = copyError;
                    return result;
                }
            }
            const std::vector<std::string> missingAssetReport = DescribeMissingRuntimeAssets(runtimeStager);
            for (const std::string& line : missingAssetReport) {
                appendLog("Warning: " + line);
            }

            {
                const fs::path buildSettingsSource = projectRoot / "build.modu";
                if (fs::exists(buildSettingsSource)) {
                    std::ifstream in(buildSettingsSource);
                    if (!in.is_open()) {
                        result.message = "Failed to read build.modu for Android export.";
                        return result;
                    }
                    std::vector<std::string> lines;
                    std::string line;
                    bool replacedSplash = false;
                    while (std::getline(in, line)) {
                        if (line.rfind("splashImage=", 0) == 0) {
                            lines.push_back("splashImage=" + stagedSplashPath);
                            replacedSplash = true;
                        } else {
                            lines.push_back(line);
                        }
                    }
                    if (!replacedSplash && !stagedSplashPath.empty()) {
                        lines.push_back("splashImage=" + stagedSplashPath);
                    }
                    const fs::path buildSettingsDest = runtimeStageRoot / "build.modu";
                    if (!EnsureDirectoryForFile(buildSettingsDest, copyError)) {
                        result.message = copyError;
                        return result;
                    }
                    std::ofstream out(buildSettingsDest, std::ios::trunc);
                    if (!out.is_open()) {
                        result.message = "Failed to write staged build.modu.";
                        return result;
                    }
                    for (const std::string& entry : lines) {
                        out << entry << "\n";
                    }
                }
            }

            std::vector<fs::path> projectFiles = {
                projectRoot / "scripts.modu",
                projectRoot / "Scripts.modu",
                projectRoot / "packages.modu"
            };
            if (!scriptsConfigPath.empty()) {
                projectFiles.push_back(scriptsConfigPath);
            }
            std::unordered_set<std::string> seenProjectFiles;
            for (const auto& src : projectFiles) {
                std::error_code srcAbsEc;
                fs::path srcAbs = fs::absolute(src, srcAbsEc);
                if (srcAbsEc) srcAbs = src;
                std::string srcKey = srcAbs.lexically_normal().string();
                if (!seenProjectFiles.insert(srcKey).second) continue;
                if (!fs::exists(src)) continue;
                if (!CopyFileIntoRuntimeRoot(src, runtimeStageRoot, src.filename(), copyError)) {
                    result.message = copyError;
                    return result;
                }
            }

            ScriptBuildConfig androidScriptConfig;
            bool haveAndroidScriptConfig = false;
            fs::path compiledScriptsSrc;
            fs::path compiledScriptsDstRelative;
            {
                std::string configError;
                if (scriptCompiler.loadConfig(scriptsConfigPath, androidScriptConfig, configError)) {
                    haveAndroidScriptConfig = true;
                    compiledScriptsSrc = androidScriptConfig.outDir;
                    if (!compiledScriptsSrc.is_absolute()) {
                        compiledScriptsSrc = projectRoot / compiledScriptsSrc;
                    }
                    std::error_code relEc;
                    fs::path relOutDir = fs::relative(compiledScriptsSrc, projectRoot, relEc);
                    if (!relEc && !relOutDir.empty()) {
                        bool hasDotDot = false;
                        for (const auto& part : relOutDir) {
                            if (part == "..") {
                                hasDotDot = true;
                                break;
                            }
                        }
                        if (!hasDotDot) {
                            compiledScriptsDstRelative = relOutDir;
                        }
                    }
                    if (compiledScriptsDstRelative.empty()) {
                        compiledScriptsDstRelative = fs::path("Library") / "CompiledScripts";
                    }
                }
            }
            if (compiledScriptsDstRelative.empty()) {
                compiledScriptsDstRelative = fs::path("Library") / "CompiledScripts";
            }
            setStatus(0.70f, "Cross-compiling scripts for Android...");
            if (haveAndroidScriptConfig) {
                std::string scriptError;
                if (!crossCompileAndroidScripts(androidScriptConfig, projectRoot, ndkRoot, androidAbi,
                                                libDir, runtimeStageRoot, compiledScriptsDstRelative,
                                                appendLog, scriptError)) {
                    result.message = scriptError;
                    return result;
                }
            } else {
                appendLog("No scripts.modu config found; skipping script cross-compilation.");
            }
            if (fs::exists(projectRoot / "Library" / "InstalledPackages")) {
                if (!CopyDirectoryIntoRuntimeRoot(projectRoot / "Library" / "InstalledPackages",
                                                  runtimeStageRoot,
                                                  fs::path("Library") / "InstalledPackages",
                                                  copyError)) {
                    result.message = copyError;
                    return result;
                }
            }

            if (compressAudio) {
                setStatus(0.74f, "Compressing audio...");
                CompressStagedAudioForExport(runtimeStageRoot, projectRoot, audioVorbisQuality, appendLog,
                                             [this]() { return exportCancelRequested.load(); });
                if (exportCancelRequested.load()) {
                    result.message = "Export cancelled.";
                    result.success = false;
                    return result;
                }
            }

            setStatus(0.76f, "Packing Android runtime content...");
            std::vector<RuntimeBundleEntry> bundleEntries = CollectRuntimeBundleEntries(runtimeStageRoot);
            if (bundleEntries.empty()) {
                result.message = "Runtime bundle staging produced no files.";
                return result;
            }
            if (!WriteRuntimeContentBundle(assetsRoot / "content.modbundle", bundleEntries, copyError)) {
                result.message = copyError;
                return result;
            }
            fs::remove_all(runtimeStageRoot, ec);
            if (ec) {
                result.message = "Failed to clean runtime staging directory.";
                return result;
            }

            std::ostringstream autoStart;
            autoStart << "bundle=content.modbundle\n";
            autoStart << "project=project.modu\n";
            if (!startScene.empty()) {
                autoStart << "scene=" << startScene << "\n";
            }
            autoStart << "mode=player\n";
            if (!writeTextFileForExport(assetsRoot / "autostart.modu", autoStart.str(), copyError)) {
                result.message = copyError;
                return result;
            }

            setStatus(0.82f, "Generating Android manifest...");
            std::string iconRef;
            if (!writeAndroidLauncherIcon(apkStageRoot / "res", sourceRoot, iconRef, copyError)) {
                result.message = copyError;
                return result;
            }
            if (!writeAndroidManifest(apkStageRoot / "AndroidManifest.xml",
                                      androidPackageName,
                                      buildNameDisplay,
                                      buildVersionDisplay,
                                      androidDebuggable,
                                      iconRef,
                                      copyError,
                                      "ModularityPlayer",
                                      34,
                                      "android.app.NativeActivity",
                                      false,
                                      androidXrOptions,
                                      projectUsesMicrophone(projectRoot))) {
                result.message = copyError;
                return result;
            }

            if (androidXrOptions.enabled) {
                setStatus(0.83f, "Staging OpenXR loader...");
                std::string loaderError;
                if (!stageAndroidOpenXrLoader(apkStageRoot, sourceRoot, androidAbi, appendLog,
                                              loaderError)) {
                    result.message = loaderError;
                    return result;
                }
            }

            setStatus(0.88f, "Packaging APK...");
            const fs::path keystorePath = exportRoot / "signing" / "debug.keystore";
            if (!packageAndroidApk(apkStageRoot, androidApkPath, androidJar,
                                   buildTools, keystorePath, appendLog, copyError)) {
                result.message = copyError;
                return result;
            }
            {
                std::vector<fs::path> looseFiles = {
                    stagedPlayerSo,
                    assetsRoot / "content.modbundle"
                };
                std::string reportError;
                if (!WriteBuildSizeReports(exportRoot, bundleEntries, looseFiles,
                                           androidApkPath, projectRoot, reportError)) {
                    appendLog("Warning: " + reportError);
                }
            }

            setStatus(1.0f, "APK export complete.");
            result.success = true;
            result.message = "Android APK export complete: " + androidApkPath.string();
            if (!missingAssetReport.empty()) {
                result.message += " (" + std::to_string(missingAssetReport.size()) +
                                  " missing asset(s) left out - see log)";
            }
            return result;
        });

        {
            std::lock_guard<std::mutex> lock(exportMutex);
            exportJob.future = std::move(future);
        }
        return;
    }

    auto future = std::async(std::launch::async,
        [this, normalizedOut, exportRoot, archivePath, sourceRoot, projectRoot, startScene,
         scenesPath, scriptsPath, scriptsConfigPath, runtimeSceneNames, exportSplashImagePath,
         assignedNativeScriptSources,
         packageStandaloneArchive, executableStem, executableFileName, packageStem,
         buildConfiguration, includeEditorInExport, shipScriptSdk, leanRuntimeExport,
         packagingMode, bundleVerifyIntegrity, obfuscateScripts, obfuscationToolCommand,
         antiTamper, antiTamperToolCommand,
         compressAudio, audioVorbisQuality, gles2Shaders, preloadShaders,
         appendCMakeModuleFlags]() {
        ExportJobResult result;
        result.outputDir = exportRoot;
        result.executableName = executableFileName;
        result.archivePath = packageStandaloneArchive ? archivePath : fs::path{};

        auto setStatus = [this](float value, const std::string& status) {
            std::lock_guard<std::mutex> lock(exportMutex);
            exportJob.progress = value;
            exportJob.status = status;
        };
        auto appendLog = [this](const std::string& text) {
            std::lock_guard<std::mutex> lock(exportMutex);
            exportJob.log += text;
            if (!exportJob.log.empty() && exportJob.log.back() != '\n') {
                exportJob.log += '\n';
            }
        };

        std::error_code ec;
        if (exportCancelRequested.load()) {
            result.message = "Export cancelled.";
            result.success = false;
            return result;
        }
        fs::create_directories(exportRoot, ec);
        if (ec) {
            result.message = "Failed to create export directory.";
            return result;
        }

        setStatus(0.05f, "Cleaning export output...");
        std::string cleanError;
        cleanExportOutput(exportRoot, executableStem.c_str(), cleanError);
        if (!cleanError.empty()) {
            result.message = cleanError;
            return result;
        }
        if (packageStandaloneArchive && fs::exists(archivePath)) {
            fs::remove(archivePath, ec);
            if (ec) {
                result.message = "Failed to remove existing archive: " + archivePath.string();
                return result;
            }
        }

        fs::path sharedBuildRoot = ModularityDevBuildDir(sourceRoot) / "player-cache";
        bool useSharedBuild = fs::exists(sharedBuildRoot / "CMakeCache.txt");
        fs::path buildRoot = useSharedBuild ? sharedBuildRoot : (normalizedOut / "_build");
        if (!useSharedBuild) {
            fs::create_directories(buildRoot, ec);
            if (ec) {
                result.message = "Failed to create build directory.";
                return result;
            }
        }
        cleanEditorExecutable(buildRoot);

        setStatus(0.1f, useSharedBuild ? "Configuring cached build..." : "Configuring build...");
        int configureExit = 0;
        std::string configureCmd = "cmake -S \"" + sourceRoot.string() + "\" -B \"" +
                                   buildRoot.string() + "\" -DCMAKE_BUILD_TYPE=" + buildConfiguration +
                                   " -DMODULARITY_BUILD_EDITOR=" + std::string(includeEditorInExport ? "ON" : "OFF");
        appendCMakeModuleFlags(configureCmd, false);
        appendLog("Running: " + configureCmd);
        if (!runCommandStreaming(configureCmd + " 2>&1", appendLog, &configureExit)) {
            result.message = "CMake configure failed (exit code " + std::to_string(configureExit) + ").";
            return result;
        }

        if (exportCancelRequested.load()) {
            result.message = "Export cancelled.";
            result.success = false;
            return result;
        }

        setStatus(0.45f, "Building...");
        int buildExit = 0;
        std::string buildCmd = "cmake --build \"" + buildRoot.string() + "\" --config " + buildConfiguration + " --target ModularityPlayer";
        appendLog("Running: " + buildCmd);
        auto onBuildChunk = [this, &appendLog](const std::string& chunk) {
            appendLog(chunk);
            // Parse lines like: "[ 17%] Building CXX object ..."
            size_t open = chunk.find('[');
            size_t pct = chunk.find('%');
            if (open != std::string::npos && pct != std::string::npos && pct > open) {
                std::string num = chunk.substr(open + 1, pct - open - 1);
                num.erase(0, num.find_first_not_of(" \t"));
                num.erase(num.find_last_not_of(" \t") + 1);
                int value = std::atoi(num.c_str());
                if (value >= 0 && value <= 100) {
                    float progress = 0.45f + (value / 100.0f) * 0.25f;
                    std::string label = "Building (" + std::to_string(value) + "%)";
                    std::lock_guard<std::mutex> lock(exportMutex);
                    exportJob.progress = progress;
                    exportJob.status = label;
                }
            }
        };
        if (!runCommandStreaming(buildCmd + " 2>&1", onBuildChunk, &buildExit)) {
            result.message = "CMake build failed (exit code " + std::to_string(buildExit) + ").";
            return result;
        }

        if (exportCancelRequested.load()) {
            result.message = "Export cancelled.";
            result.success = false;
            return result;
        }

        setStatus(0.7f, "Copying runtime...");
        fs::path exePath = resolveExecutablePath(buildRoot, "ModularityPlayer");
        if (exePath.empty()) {
            result.message = "Built executable not found.";
            return result;
        }

        fs::path destExe = exportRoot / executableFileName;
        fs::copy_file(exePath, destExe, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            result.message = "Failed to copy executable.";
            return result;
        }

        std::string copyError;

#ifdef _WIN32
        {
            const fs::path importLibSource = exePath.parent_path() / (exePath.stem().string() + ".lib");
            if (fs::exists(importLibSource)) {
                fs::copy_file(importLibSource,
                              exportRoot / importLibSource.filename(),
                              fs::copy_options::overwrite_existing,
                              ec);
                if (ec) {
                    result.message = "Failed to copy player import library.";
                    return result;
                }
            }
        }

        {
            std::unordered_set<std::string> copiedDlls;
            std::error_code dllEc;
            const fs::path runtimeDllSourceDir = exePath.parent_path();
            if (fs::exists(runtimeDllSourceDir, dllEc)) {
                for (auto it = fs::recursive_directory_iterator(runtimeDllSourceDir, dllEc);
                     it != fs::recursive_directory_iterator(); ++it) {
                    if (dllEc) break;
                    if (!it->is_regular_file()) continue;

                    fs::path sourceDll = it->path();
                    std::string ext = sourceDll.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (ext != ".dll") continue;

                    fs::path destinationDll = exportRoot / sourceDll.filename();
                    std::string destinationKey = destinationDll.lexically_normal().string();
                    if (!copiedDlls.insert(destinationKey).second) continue;

                    fs::copy_file(sourceDll, destinationDll, fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        result.message = "Failed to copy runtime DLL: " + sourceDll.filename().string();
                        return result;
                    }
                }
            }
        }
#else
        {
#if defined(__APPLE__)
            const fs::path discordRuntime = exePath.parent_path() / "libdiscord_partner_sdk.dylib";
#else
            const fs::path discordRuntime = exePath.parent_path() / "libdiscord_partner_sdk.so";
#endif
            if (fs::exists(discordRuntime, ec)) {
                fs::copy_file(discordRuntime, exportRoot / discordRuntime.filename(),
                              fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    result.message = "Failed to copy Discord Social SDK runtime.";
                    return result;
                }
            }
        }
#endif

        WriteShippedLibraryLicenses(exportRoot, sourceRoot, appendLog);

        {
            fs::path scriptSdkSource = sourceRoot / "Resources" / "ScriptSDK";
            if (!fs::exists(scriptSdkSource)) {
                scriptSdkSource = buildRoot / "Resources" / "ScriptSDK";
            }
            if (shipScriptSdk && fs::exists(scriptSdkSource)) {
                if (!CopyDirectoryIntoRuntimeRoot(scriptSdkSource,
                                                  exportRoot,
                                                  fs::path("Resources") / "ScriptSDK",
                                                  copyError)) {
                    result.message = copyError;
                    return result;
                }
            }
        }

        fs::path runtimeStageRoot = exportRoot / "_runtime_stage";
        fs::create_directories(runtimeStageRoot, ec);
        if (ec) {
            result.message = "Failed to create runtime staging directory.";
            return result;
        }

        RuntimeExportStager runtimeStager(projectRoot, runtimeStageRoot);

        setStatus(0.74f, "Staging runtime resources...");
        if (!CopyRuntimeResourcesSubset(sourceRoot, runtimeStageRoot, leanRuntimeExport, copyError)) {
            result.message = copyError;
            return result;
        }

        setStatus(0.78f, "Collecting precompiled packages...");
        if (!copyPrecompiledPackages(buildRoot, runtimeStageRoot / "Packages" / "ThirdParty", leanRuntimeExport, copyError)) {
            result.message = copyError;
            return result;
        }

        setStatus(0.82f, "Collecting engine cache...");
        if (!copyPrecompiledEnginePackages(buildRoot, runtimeStageRoot / "Packages" / "Engine", leanRuntimeExport, copyError)) {
            result.message = copyError;
            return result;
        }

        {
            ScriptBuildConfig scriptConfig;
            std::string configError;
            if (scriptCompiler.loadConfig(scriptsConfigPath, scriptConfig, configError)) {
                auto isNativeSourceFile = [](const fs::path& sourcePath) {
                    std::string ext = sourcePath.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
                           ext == ".c++" || ext == ".moducpp" || ext == ".mko" ||
                           ext == ".modumako";
                };

                packageManager.applyToBuildConfig(scriptConfig);

                auto resolveAssignedSource = [&](const fs::path& input) -> fs::path {
                    if (input.empty()) return {};
                    std::error_code sourceEc;
                    fs::path abs = fs::absolute(input, sourceEc);
                    if (sourceEc) abs = input;
                    if (fs::exists(abs)) return abs;

                    fs::path scriptsDir = scriptConfig.scriptsDir;
                    if (!scriptsDir.is_absolute()) {
                        scriptsDir = projectRoot / scriptsDir;
                    }

                    if (input.is_relative()) {
                        fs::path candidate = projectRoot / input;
                        if (fs::exists(candidate)) return candidate;
                    }

                    auto remapSuffix = [&](const fs::path& marker) -> fs::path {
                        std::vector<fs::path> parts;
                        for (const auto& p : abs) parts.push_back(p);
                        std::vector<fs::path> markerParts;
                        for (const auto& p : marker) markerParts.push_back(p);
                        if (markerParts.empty()) return {};
                        for (size_t i = 0; i + markerParts.size() <= parts.size(); ++i) {
                            bool match = true;
                            for (size_t k = 0; k < markerParts.size(); ++k) {
                                if (parts[i + k] != markerParts[k]) {
                                    match = false;
                                    break;
                                }
                            }
                            if (match) {
                                fs::path suffix;
                                for (size_t j = i + markerParts.size(); j < parts.size(); ++j) {
                                    suffix /= parts[j];
                                }
                                if (!suffix.empty()) {
                                    fs::path candidate = scriptsDir / suffix;
                                    if (fs::exists(candidate)) return candidate;
                                }
                                break;
                            }
                        }
                        return {};
                    };

                    fs::path remapped = remapSuffix(fs::path("Assets") / "Scripts");
                    if (!remapped.empty()) return remapped;
                    remapped = remapSuffix("Scripts");
                    if (!remapped.empty()) return remapped;

                    if (!abs.filename().empty()) {
                        fs::path candidate = scriptsDir / abs.filename();
                        if (fs::exists(candidate)) return candidate;
                    }
                    return {};
                };

                std::vector<fs::path> nativeSourcesToCompile;
                std::unordered_set<std::string> seenNativeSources;
                auto addNativeSource = [&](const fs::path& sourcePath) {
                    if (sourcePath.empty()) return;
                    std::error_code absEc;
                    fs::path abs = fs::absolute(sourcePath, absEc);
                    if (absEc) abs = sourcePath;
                    std::string key = abs.lexically_normal().string();
                    if (seenNativeSources.insert(key).second) {
                        nativeSourcesToCompile.push_back(abs);
                    }
                };

                for (const fs::path& sourceRef : assignedNativeScriptSources) {
                    fs::path resolvedSource = resolveAssignedSource(sourceRef);
                    if (resolvedSource.empty()) {
                        appendLog("Warning: Native script source not found during export scan: " +
                                  sourceRef.string());
                        continue;
                    }
                    if (isNativeSourceFile(resolvedSource)) {
                        addNativeSource(resolvedSource);
                    }
                }

                std::vector<fs::path> scriptScanRoots;
                scriptScanRoots.push_back(projectRoot);
                scriptScanRoots.push_back(scriptConfig.scriptsDir);
                if (!scriptsPath.empty()) {
                    scriptScanRoots.push_back(scriptsPath);
                }

                std::error_code outDirAbsEc;
                fs::path absOutDir = fs::absolute(scriptConfig.outDir, outDirAbsEc);
                if (outDirAbsEc) absOutDir = scriptConfig.outDir;
                const std::string outDirKey = absOutDir.lexically_normal().string();
                auto isGeneratedScriptSource = [](const fs::path& path) {
                    const std::string name = path.filename().string();
                    return name.find(".gen.cpp") != std::string::npos ||
                           name.find(".gen.c") != std::string::npos ||
                           name.find(".wrap.cpp") != std::string::npos;
                };

                std::unordered_set<std::string> seenScanRoots;
                for (const auto& scanRoot : scriptScanRoots) {
                    std::error_code absEc;
                    fs::path absRoot = fs::absolute(scanRoot, absEc);
                    if (absEc) absRoot = scanRoot;
                    std::string rootKey = absRoot.lexically_normal().string();
                    if (!seenScanRoots.insert(rootKey).second) continue;

                    std::error_code scriptsEc;
                    if (!fs::exists(absRoot, scriptsEc)) continue;

                    for (auto it = fs::recursive_directory_iterator(absRoot, scriptsEc);
                         it != fs::recursive_directory_iterator(); ++it) {
                        if (scriptsEc) break;
                        if (it->is_directory()) {
                            const fs::path dirPath = it->path();
                            std::error_code dirEc;
                            fs::path absDir = fs::absolute(dirPath, dirEc);
                            if (dirEc) absDir = dirPath;
                            if (isScriptScanExcludedDir(dirPath.filename().string(), it.depth() == 0) ||
                                absDir.lexically_normal().string() == outDirKey) {
                                it.disable_recursion_pending();
                            }
                            continue;
                        }
                        if (!it->is_regular_file()) continue;
                        const fs::path sourcePath = it->path();
                        if (!isNativeSourceFile(sourcePath)) continue;
                        if (isGeneratedScriptSource(sourcePath)) continue;
                        addNativeSource(sourcePath);
                    }
                }

                if (!nativeSourcesToCompile.empty()) {
                    setStatus(0.84f, "Compiling native scripts...");
                    for (const fs::path& resolvedSource : nativeSourcesToCompile) {
                        if (exportCancelRequested.load()) {
                            result.message = "Export cancelled.";
                            result.success = false;
                            return result;
                        }

                        ScriptBuildCommands commands;
                        std::string buildError;
                        if (!scriptCompiler.makeCommands(scriptConfig, resolvedSource, commands, buildError)) {
                            result.message = "Failed to prepare native script build for '" +
                                             resolvedSource.filename().string() + "': " + buildError;
                            return result;
                        }

                        appendLog("Compiling native script: " + resolvedSource.string());
                        ScriptCompileOutput compileOutput;
                        if (!scriptCompiler.compile(commands, compileOutput, buildError)) {
                            if (!compileOutput.compileLog.empty()) appendLog(compileOutput.compileLog);
                            if (!compileOutput.linkLog.empty()) appendLog(compileOutput.linkLog);
                            result.message = "Failed to compile native script '" +
                                             resolvedSource.filename().string() + "': " + buildError;
                            return result;
                        }
                        if (!compileOutput.compileLog.empty()) appendLog(compileOutput.compileLog);
                        if (!compileOutput.linkLog.empty()) appendLog(compileOutput.linkLog);
                    }
                }
            } else if (!assignedNativeScriptSources.empty()) {
                result.message = "Failed to load scripts config for export: " + configError;
                return result;
            }
        }

        setStatus(0.85f, "Staging runtime project...");
        if (!CopyFileIntoRuntimeRoot(projectRoot / "project.modu", runtimeStageRoot, "project.modu", copyError)) {
            result.message = copyError;
            return result;
        }

        RuntimeShaderPairs runtimeShaderPairs;
        if (!runtimeSceneNames.empty()) {
            const float sceneCount = static_cast<float>(runtimeSceneNames.size());
            for (size_t i = 0; i < runtimeSceneNames.size(); ++i) {
                if (exportCancelRequested.load()) {
                    result.message = "Export cancelled.";
                    result.success = false;
                    return result;
                }

                const std::string& sceneName = runtimeSceneNames[i];
                const fs::path sourceScenePath = scenesPath / (sceneName + ".scene");
                const float progressBase = 0.86f;
                const float progressRange = 0.04f;
                const float progress = progressBase +
                    (progressRange * static_cast<float>(i) / std::max(1.0f, sceneCount));
                setStatus(progress, "Serializing runtime scene: " + sceneName + "...");
                if (!StageRuntimeScene(sourceScenePath, sceneName, runtimeStager, runtimeStageRoot, copyError,
                                       &runtimeShaderPairs)) {
                    result.message = copyError;
                    return result;
                }
            }
        }
        if (!StageRuntimeShaderPrograms(runtimeStageRoot, runtimeShaderPairs, gles2Shaders, preloadShaders,
                                        appendLog, copyError)) {
            result.message = copyError;
            return result;
        }

        std::string stagedSplashPath = exportSplashImagePath;
        if (!stagedSplashPath.empty()) {
            runtimeStager.setReferenceOwner("Build settings");
            runtimeStager.setReferenceLabel("splash image");
            if (!runtimeStager.stageFileReference(stagedSplashPath, "Splash", copyError)) {
                result.message = copyError;
                return result;
            }
        }
        const std::vector<std::string> missingAssetReport = DescribeMissingRuntimeAssets(runtimeStager);
        for (const std::string& line : missingAssetReport) {
            appendLog("Warning: " + line);
        }

        {
            const fs::path buildSettingsSource = projectRoot / "build.modu";
            if (fs::exists(buildSettingsSource)) {
                std::ifstream in(buildSettingsSource);
                if (!in.is_open()) {
                    result.message = "Failed to read build.modu for runtime export.";
                    return result;
                }

                std::vector<std::string> lines;
                std::string line;
                bool replacedSplash = false;
                while (std::getline(in, line)) {
                    if (line.rfind("splashImage=", 0) == 0) {
                        lines.push_back("splashImage=" + stagedSplashPath);
                        replacedSplash = true;
                    } else {
                        lines.push_back(line);
                    }
                }
                if (!replacedSplash && !stagedSplashPath.empty()) {
                    lines.push_back("splashImage=" + stagedSplashPath);
                }

                const fs::path buildSettingsDest = runtimeStageRoot / "build.modu";
                if (!EnsureDirectoryForFile(buildSettingsDest, copyError)) {
                    result.message = copyError;
                    return result;
                }
                std::ofstream out(buildSettingsDest, std::ios::trunc);
                if (!out.is_open()) {
                    result.message = "Failed to write staged build.modu.";
                    return result;
                }
                for (const std::string& entry : lines) {
                    out << entry << "\n";
                }
                out.close();
            }
        }

        std::vector<fs::path> projectFiles = {
            projectRoot / "scripts.modu",
            projectRoot / "Scripts.modu",
            projectRoot / "packages.modu"
        };
        if (!scriptsConfigPath.empty()) {
            projectFiles.push_back(scriptsConfigPath);
        }
        std::unordered_set<std::string> seenProjectFiles;
        for (const auto& src : projectFiles) {
            std::error_code srcAbsEc;
            fs::path srcAbs = fs::absolute(src, srcAbsEc);
            if (srcAbsEc) srcAbs = src;
            std::string srcKey = srcAbs.lexically_normal().string();
            if (!seenProjectFiles.insert(srcKey).second) continue;
            if (!fs::exists(src)) continue;
            if (!CopyFileIntoRuntimeRoot(src, runtimeStageRoot, src.filename(), copyError)) {
                result.message = copyError;
                return result;
            }
        }

        fs::path compiledScriptsSrc;
        fs::path compiledScriptsDstRelative;
        {
            ScriptBuildConfig scriptConfig;
            std::string configError;
            if (scriptCompiler.loadConfig(scriptsConfigPath, scriptConfig, configError)) {
                compiledScriptsSrc = scriptConfig.outDir;
                if (!compiledScriptsSrc.is_absolute()) {
                    compiledScriptsSrc = projectRoot / compiledScriptsSrc;
                }
                std::error_code relEc;
                fs::path relOutDir = fs::relative(compiledScriptsSrc, projectRoot, relEc);
                if (!relEc && !relOutDir.empty()) {
                    bool hasDotDot = false;
                    for (const auto& part : relOutDir) {
                        if (part == "..") {
                            hasDotDot = true;
                            break;
                        }
                    }
                    if (!hasDotDot) {
                        compiledScriptsDstRelative = relOutDir;
                    }
                }
                if (compiledScriptsDstRelative.empty()) {
                    compiledScriptsDstRelative = fs::path("Library") / "CompiledScripts";
                }
            }
        }
        if (compiledScriptsSrc.empty()) {
            compiledScriptsSrc = projectRoot / "Library" / "CompiledScripts";
            compiledScriptsDstRelative = fs::path("Library") / "CompiledScripts";
        }
        RuntimeCopySkips scriptBuildLeftovers;
        {
            // Exports do not clear their folder, so remove the loose copy an older export left.
            // Only a real subfolder that still looks like one (compiler dot-folders inside) - an
            // outDir of the project root itself would otherwise make this the export folder.
            const fs::path relative = compiledScriptsDstRelative.lexically_normal();
            const fs::path staleLooseScripts = exportRoot / relative;
            std::error_code staleEc;
            const bool isSubfolder = !relative.empty() && relative != "." &&
                                     *relative.begin() != "..";
            const bool looksLikeScriptOutput =
                fs::is_directory(staleLooseScripts / ".pch", staleEc) ||
                fs::is_directory(staleLooseScripts / ".staging", staleEc);
            if (isSubfolder && looksLikeScriptOutput) {
                fs::remove_all(staleLooseScripts, staleEc);
                if (!staleEc) {
                    appendLog("Removed stale loose script folder from a previous export: " +
                              staleLooseScripts.string());
                }
            }
        }
        if (fs::exists(compiledScriptsSrc)) {
            // Scripts load from the extracted bundle, so only the staged copy is used. A second,
            // loose copy next to the exe (from before scripts loaded in builds at all) shipped
            // every compiler leftover twice.
            if (!CopyCompiledScriptBinaries(compiledScriptsSrc, runtimeStageRoot, compiledScriptsDstRelative,
                                            copyError, &scriptBuildLeftovers)) {
                result.message = copyError;
                return result;
            }
        }
        if (fs::exists(projectRoot / "Library" / "InstalledPackages")) {
            if (!CopyDirectoryIntoRuntimeRoot(projectRoot / "Library" / "InstalledPackages",
                                              runtimeStageRoot,
                                              fs::path("Library") / "InstalledPackages",
                                              copyError)) {
                result.message = copyError;
                return result;
            }
        }

        {
            const fs::path managedProject = projectRoot / "Scripts" / "Managed" / "ModuCPP.csproj";
            if (fs::exists(managedProject)) {
                if (!CopyFileIntoRuntimeRoot(managedProject,
                                             runtimeStageRoot,
                                             fs::path("Scripts") / "Managed" / "ModuCPP.csproj",
                                             copyError)) {
                    result.message = copyError;
                    return result;
                }

                const fs::path managedOutputDir = managedOutputPathFromProject(managedProject).parent_path();
                if (fs::exists(managedOutputDir)) {
                    if (!CopyDirectoryIntoRuntimeRoot(managedOutputDir,
                                                      runtimeStageRoot,
                                                      fs::path("Scripts") / "Managed" / "bin" / "Debug" / "netstandard2.0",
                                                      copyError)) {
                        result.message = copyError;
                        return result;
                    }
                }
            }
        }

        if (compressAudio) {
            setStatus(0.90f, "Compressing audio...");
            CompressStagedAudioForExport(runtimeStageRoot, projectRoot, audioVorbisQuality, appendLog,
                                         [this]() { return exportCancelRequested.load(); });
            if (exportCancelRequested.load()) {
                result.message = "Export cancelled.";
                result.success = false;
                return result;
            }
        }

        setStatus(0.92f, "Packing runtime content...");
        std::vector<RuntimeBundleEntry> bundleEntries = CollectRuntimeBundleEntries(runtimeStageRoot);
        {
            // copyPrecompiledPackages gathers every binary in the player build tree, and the same
            // files are already copied next to the exe, which is where they load from - nothing
            // adds Packages/ to a library search path. Bundling them again doubled ~260 MB of
            // ffmpeg. Only exact duplicates are dropped.
            RuntimeCopySkips duplicatePackages;
            bundleEntries.erase(
                std::remove_if(bundleEntries.begin(), bundleEntries.end(),
                               [&](const RuntimeBundleEntry& entry) {
                                   if (entry.archivePath.generic_string().rfind("Packages/", 0) != 0) {
                                       return false;
                                   }
                                   const fs::path loose = exportRoot / entry.archivePath.filename();
                                   if (!FilesHaveSameContents(entry.sourcePath, loose)) return false;
                                   ++duplicatePackages.files;
                                   std::error_code sizeEc;
                                   const auto size = fs::file_size(loose, sizeEc);
                                   if (!sizeEc) duplicatePackages.bytes += size;
                                   return true;
                               }),
                bundleEntries.end());

            auto describe = [](const RuntimeCopySkips& skips) {
                char text[64];
                std::snprintf(text, sizeof(text), "%llu files, %.1f MB",
                              static_cast<unsigned long long>(skips.files),
                              static_cast<double>(skips.bytes) / (1024.0 * 1024.0));
                return std::string(text);
            };
            if (scriptBuildLeftovers.files > 0) {
                appendLog("Left out script build leftovers (precompiled header, objects, debug symbols): " +
                          describe(scriptBuildLeftovers));
            }
            if (runtimeStager.skippedJunk().files > 0) {
                appendLog("Left out non-runtime files from asset folders (.meta, unfinished downloads, "
                          "Blender backups): " + describe(runtimeStager.skippedJunk()));
            }
            if (duplicatePackages.files > 0) {
                appendLog("Left out package binaries already shipped next to the executable: " +
                          describe(duplicatePackages));
            }
        }
        if (bundleEntries.empty()) {
            result.message = "Runtime bundle staging produced no files.";
            return result;
        }

        // Packaging mode decides how runtime content ships. The direct loaders (project.modu and
        // scene files via std::ifstream, script binaries via LoadLibrary/dlopen) cannot read out of
        // a mounted bundle, so in "mount" they stay loose on disk while the bulk assets live in the
        // single file that the engine reads in place (data.win style).
        auto isDirectLoad = [](const fs::path& archivePath) {
            std::string ext = archivePath.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".modu" || ext == ".scene" || ext == ".dll" ||
                   ext == ".so" || ext == ".dylib";
        };
        auto copyEntriesLoose = [&](const std::vector<RuntimeBundleEntry>& list) -> bool {
            for (const RuntimeBundleEntry& e : list) {
                const fs::path dest = exportRoot / e.archivePath;
                std::error_code cec;
                fs::create_directories(dest.parent_path(), cec);
                fs::copy_file(e.sourcePath, dest, fs::copy_options::overwrite_existing, cec);
                if (cec) {
                    copyError = "Failed to copy loose runtime file: " + e.archivePath.generic_string() +
                                " (" + cec.message() + ")";
                    return false;
                }
            }
            return true;
        };

        const fs::path bundleFile = exportRoot / "content.modbundle";
        std::string autoStartBundleLine;   // empty in loose mode (no bundle= key)
        std::string autoStartModeLines;    // bundleMode / verifyIntegrity keys
        std::vector<RuntimeBundleEntry> reportBundleEntries = bundleEntries;

        if (packagingMode == "loose") {
            setStatus(0.94f, "Copying loose runtime content...");
            if (!copyEntriesLoose(bundleEntries)) { result.message = copyError; return result; }
            reportBundleEntries.clear();
        } else if (packagingMode == "mount") {
            setStatus(0.94f, "Packing mounted bundle...");
            std::vector<RuntimeBundleEntry> looseList, bundleList;
            for (const RuntimeBundleEntry& e : bundleEntries) {
                (isDirectLoad(e.archivePath) ? looseList : bundleList).push_back(e);
            }
            if (!copyEntriesLoose(looseList)) { result.message = copyError; return result; }
            if (bundleList.empty()) {
                result.message = "Mounted bundle produced no asset files to pack.";
                return result;
            }
            RuntimeBundleWriteOptions opt;
            opt.includeIntegrity = bundleVerifyIntegrity;
            // The CODE section is reserved for future ModuCPP bytecode; empty for now.
            std::vector<RuntimeBundleCodeUnit> codeUnits;
            if (!WriteRuntimeContentBundleV2(bundleFile, bundleList, codeUnits, opt, copyError)) {
                result.message = copyError;
                return result;
            }
            autoStartBundleLine = "bundle=content.modbundle\n";
            autoStartModeLines = std::string("bundleMode=mount\n") +
                                 "verifyIntegrity=" + (bundleVerifyIntegrity ? "1" : "0") + "\n";
            reportBundleEntries = bundleList;
            appendLog("Packaged " + std::to_string(bundleList.size()) + " asset(s) into a mounted bundle; " +
                      std::to_string(looseList.size()) + " project/scene/script file(s) left loose.");
        } else {  // "extract" (default): the proven v1 unpack-on-first-run path.
            setStatus(0.94f, "Packing runtime bundle...");
            if (!WriteRuntimeContentBundle(bundleFile, bundleEntries, copyError)) {
                result.message = copyError;
                return result;
            }
            autoStartBundleLine = "bundle=content.modbundle\n";
            autoStartModeLines = "bundleMode=extract\n";
        }

        {
            std::vector<fs::path> looseFiles = { destExe };
            if (!autoStartBundleLine.empty()) looseFiles.push_back(bundleFile);
            std::string reportError;
            if (!WriteBuildSizeReports(exportRoot, reportBundleEntries, looseFiles, {},
                                       projectRoot, reportError)) {
                appendLog("Warning: " + reportError);
            }
        }

        fs::remove_all(runtimeStageRoot, ec);
        if (ec) {
            result.message = "Failed to clean runtime staging directory.";
            return result;
        }

        fs::path autoStartPath = exportRoot / "autostart.modu";
        std::ofstream autoStart(autoStartPath);
        if (!autoStart.is_open()) {
            result.message = "Failed to write autostart.modu.";
            return result;
        }
        autoStart << autoStartBundleLine;
        autoStart << autoStartModeLines;
        autoStart << "project=project.modu\n";
        if (!startScene.empty()) {
            autoStart << "scene=" << startScene << "\n";
        }
        autoStart << "mode=player\n";
        autoStart.close();

        // Protection post-build steps. These invoke the developer's own tools (Obscura for script
        // obfuscation, NativeShield / RMG for anti-tamper) over the staged output. {exe}, {bundle}
        // and {dir} are substituted; a non-zero exit fails the export so a "protected" build never
        // ships unprotected without notice.
        {
            auto runProtectionTool = [&](const char* label, bool enabled,
                                         const std::string& cmdTemplate) -> bool {
                if (!enabled) return true;
                if (cmdTemplate.empty()) {
                    appendLog(std::string("Warning: ") + label +
                              " is enabled but no tool command is set; skipping.");
                    return true;
                }
                std::string cmd = cmdTemplate;
                auto sub = [&](const std::string& token, const std::string& value) {
                    for (size_t p = cmd.find(token); p != std::string::npos; p = cmd.find(token, p)) {
                        cmd.replace(p, token.size(), value);
                        p += value.size();
                    }
                };
                sub("{exe}", quotePath(destExe));
                sub("{bundle}", quotePath(autoStartBundleLine.empty() ? exportRoot : bundleFile));
                sub("{dir}", quotePath(exportRoot));
                appendLog(std::string("Running ") + label + ": " + cmd);
                int toolExit = 0;
                if (!runCommandStreaming(cmd + " 2>&1", appendLog, &toolExit) || toolExit != 0) {
                    result.message = std::string(label) + " failed (exit code " +
                                     std::to_string(toolExit) + ").";
                    return false;
                }
                return true;
            };
            setStatus(0.96f, "Applying protection...");
            if (!runProtectionTool("Script obfuscation (Obscura)", obfuscateScripts,
                                   obfuscationToolCommand)) {
                return result;
            }
            if (!runProtectionTool("Anti-tamper (NativeShield/RMG)", antiTamper,
                                   antiTamperToolCommand)) {
                return result;
            }
        }

        if (packageStandaloneArchive) {
#ifdef _WIN32
            appendLog("Standalone archive packaging is skipped on Windows exports.");
            result.archivePath.clear();
#else
            setStatus(0.97f, "Packing standalone archive...");
            int archiveExit = 0;
            std::string archiveCmd = "cd " + quotePath(normalizedOut) + " && tar -czf " +
                                     quotePath(archivePath.filename()) + " " +
                                     quotePath(fs::path(packageStem));
            appendLog("Running: " + archiveCmd);
            if (!runCommandStreaming(archiveCmd + " 2>&1", appendLog, &archiveExit)) {
                result.message = "Failed to create standalone archive (exit code " + std::to_string(archiveExit) + ").";
                return result;
            }
#endif
        }

        if (!useSharedBuild) {
            std::error_code cleanupEc;
            fs::remove_all(buildRoot, cleanupEc);
            if (cleanupEc) {
                appendLog("Warning: Failed to remove temporary build directory: " + buildRoot.string());
            }
        }

        setStatus(1.0f, "Export complete.");
        result.success = true;
        if (packageStandaloneArchive && !result.archivePath.empty()) {
            result.message = "Export complete. Archive: " + result.archivePath.string();
        } else {
            result.message = "Export complete.";
        }
        if (!missingAssetReport.empty()) {
            result.message += " " + std::to_string(missingAssetReport.size()) +
                              " missing asset(s) were left out - see the log.";
        }
        return result;
    });

    {
        std::lock_guard<std::mutex> lock(exportMutex);
        exportJob.future = std::move(future);
    }
}

void Engine::pollExportBuild() {
    if (!exportJob.active) return;
    if (!exportJob.future.valid()) {
        exportJob.active = false;
        return;
    }
    auto state = exportJob.future.wait_for(std::chrono::milliseconds(0));
    if (state != std::future_status::ready) return;

    ExportJobResult result = exportJob.future.get();
    {
        std::lock_guard<std::mutex> lock(exportMutex);
        exportJob.done = true;
        exportJob.active = false;
        exportJob.success = result.success;
        exportJob.status = result.message;
        exportJob.outputDir = result.outputDir;
        exportJob.executableName = result.executableName;
        exportJob.archivePath = result.archivePath;
        exportJob.cancelled = exportCancelRequested.load() && !result.success;
    }

    bool runAfter = false;
    std::string executableName;
    {
        std::lock_guard<std::mutex> lock(exportMutex);
        runAfter = exportJob.runAfter;
        executableName = exportJob.executableName;
    }

    if (result.success) {
        addConsoleMessage("Export finished: " + result.outputDir.string(), ConsoleMessageType::Success);
        if (runAfter) {
            if (executableName.empty()) {
                executableName =
#ifdef _WIN32
                    "ModularityPlayer.exe";
#else
                    "ModularityPlayer";
#endif
            }
            fs::path exePath = result.outputDir / executableName;
            if (fs::exists(exePath)) {
#ifdef _WIN32
                std::string runCmd = "start \"\" \"" + exePath.string() + "\"";
#else
                std::string runCmd = "\"" + exePath.string() + "\" &";
#endif
                std::string runOut;
                runCommandCapture(runCmd + " 2>&1", runOut);
            } else {
                addConsoleMessage("Export finished, but executable was not found to run.", ConsoleMessageType::Warning);
            }
        }
    } else if (exportJob.cancelled) {
        addConsoleMessage("Export cancelled.", ConsoleMessageType::Warning);
    } else {
        addConsoleMessage("Export failed: " + result.message, ConsoleMessageType::Error);
    }
}

int Engine::buildAndroidApkHeadless(const AndroidBuildRequest& request, std::string& error) {
    if (!request.abi.empty() && !AndroidToolchain::ValidAbi(request.abi)) {
        error = "Unsupported Android ABI: " + request.abi;
        return 2;
    }
    if (request.projectPath.empty() || request.editor) {
        return buildBareAndroidApk(request, error);
    }

    std::error_code ec;
    fs::path projectFile = fs::absolute(request.projectPath, ec);
    if (ec) projectFile = request.projectPath;
    if (!fs::exists(projectFile, ec) || ec) {
        error = "Project file not found: " + projectFile.string();
        return 2;
    }
    if (!projectManager.loadProject(projectFile.string())) {
        error = projectManager.errorMessage.empty()
            ? ("Failed to load project: " + projectFile.string())
            : projectManager.errorMessage;
        return 2;
    }
    loadBuildSettings();
    buildSettings.platform = BuildPlatform::Android;
    if (!request.abi.empty()) buildSettings.architecture = request.abi;
    if (request.debug) buildSettings.developmentBuild = true;

    fs::path outputDir = request.outputDir.empty()
        ? (projectManager.currentProject.projectPath / "Build" / "Android")
        : fs::path(request.outputDir);
    fs::create_directories(outputDir, ec);

    std::fprintf(stderr, "[Android] Building APK for project '%s' (abi=%s)...\n",
                 projectManager.currentProject.name.c_str(),
                 buildSettings.architecture.c_str());

    startExportBuild(outputDir, false);
    bool active = true;
    while (active) {
        pollExportBuild();
        {
            std::lock_guard<std::mutex> lock(exportMutex);
            active = exportJob.active;
        }
        if (active) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bool ok = false;
    std::string status;
    std::string log;
    fs::path producedApk;
    {
        std::lock_guard<std::mutex> lock(exportMutex);
        ok = exportJob.success;
        status = exportJob.status;
        log = exportJob.log;
        producedApk = exportJob.archivePath;
    }
    if (!log.empty()) std::fprintf(stderr, "%s\n", log.c_str());
    if (!ok) {
        error = status.empty() ? "Android export failed." : status;
        return 1;
    }

    if (!request.outputApk.empty() && !producedApk.empty()) {
        fs::path dest = fs::absolute(request.outputApk, ec);
        if (ec) dest = request.outputApk;
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(producedApk, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Built APK but failed to copy to " + dest.string() + ": " + ec.message();
            return 1;
        }
        std::fprintf(stdout, "Android APK: %s\n", dest.string().c_str());
    } else {
        std::fprintf(stdout, "Android APK: %s\n", producedApk.string().c_str());
    }
    return 0;
}

int Engine::buildDesktopHeadless(const std::string& projectPath, const std::string& outputDir, std::string& error) {
    std::error_code ec;
    fs::path projectFile = fs::absolute(projectPath, ec);
    if (ec) projectFile = projectPath;
    if (projectPath.empty() || !fs::exists(projectFile, ec) || ec) {
        error = "Project file not found: " + projectFile.string();
        return 2;
    }
    if (!projectManager.loadProject(projectFile.string())) {
        error = projectManager.errorMessage.empty()
            ? ("Failed to load project: " + projectFile.string())
            : projectManager.errorMessage;
        return 2;
    }
    loadBuildSettings();
#ifdef _WIN32
    buildSettings.platform = BuildPlatform::Windows;
#else
    buildSettings.platform = BuildPlatform::Linux;
#endif

    fs::path output = outputDir.empty()
        ? (projectManager.currentProject.projectPath / "Builds")
        : fs::path(outputDir);
    fs::create_directories(output, ec);
    std::fprintf(stderr, "[Build] Exporting project '%s' to %s...\n",
                 projectManager.currentProject.name.c_str(), output.string().c_str());

    startExportBuild(output, false);
    bool active = true;
    while (active) {
        pollExportBuild();
        {
            std::lock_guard<std::mutex> lock(exportMutex);
            active = exportJob.active;
        }
        if (active) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bool ok = false;
    std::string status;
    std::string log;
    {
        std::lock_guard<std::mutex> lock(exportMutex);
        ok = exportJob.success;
        status = exportJob.status;
        log = exportJob.log;
    }
    if (!log.empty()) std::fprintf(stderr, "%s\n", log.c_str());
    {
        // Modularity.exe is a GUI-subsystem program, so a shell usually shows none of stderr.
        std::ofstream logFile(output / "export-log.txt", std::ios::trunc);
        logFile << log << "\n" << (ok ? "Export succeeded: " : "Export failed: ") << status << "\n";
    }
    if (!ok) {
        error = status.empty() ? "Desktop export failed." : status;
        return 1;
    }
    std::fprintf(stdout, "Export complete: %s\n", status.c_str());
    return 0;
}

int Engine::buildBareAndroidApk(const AndroidBuildRequest& request, std::string& error) {
    std::error_code ec;
    const std::string abi = request.abi.empty() ? std::string("arm64-v8a") : request.abi;

    const fs::path ndkRoot = resolveAndroidNdkPath();
    if (ndkRoot.empty()) {
        error = "Android NDK not found. Set ANDROID_NDK_ROOT, ANDROID_NDK_HOME, or ANDROID_NDK.";
        return 2;
    }
    const fs::path toolchainFile = ndkRoot / "build" / "cmake" / "android.toolchain.cmake";
    if (!fs::exists(toolchainFile, ec) || ec) {
        error = "Android NDK missing android.toolchain.cmake: " + toolchainFile.string();
        return 2;
    }
    const fs::path sdkRoot = findAndroidSdkRoot();
    if (sdkRoot.empty()) {
        error = "Android SDK not found. Set ANDROID_SDK_ROOT or ANDROID_HOME.";
        return 2;
    }

    fs::path sourceRoot;
    {
        std::vector<fs::path> candidates;
#ifdef MODULARITY_SOURCE_DIR
        candidates.emplace_back(MODULARITY_SOURCE_DIR);
#endif
        candidates.push_back(fs::current_path());
        fs::path exe = resolveCurrentExecutablePath();
        if (!exe.empty()) {
            candidates.push_back(exe.parent_path());
            candidates.push_back(exe.parent_path().parent_path());
        }
        for (const auto& c : candidates) {
            sourceRoot = findCMakeSourceRoot(c);
            if (!sourceRoot.empty()) break;
        }
    }
    if (sourceRoot.empty()) {
        error = "Could not locate the engine's CMakeLists.txt for the Android build.";
        return 2;
    }

    std::vector<fs::path> jarRoots = { sdkRoot };
    const fs::path localSdkRoot = ModularityAndroidDir(sourceRoot, "android-sdk");
    for (const fs::path& localSdk : { localSdkRoot,
                                      ModularityAndroidDir(sourceRoot, "android-sdk-smoke") }) {
        if (localSdk != sdkRoot) jarRoots.push_back(localSdk);
    }
    fs::path jarSdkRoot;
    int targetApi = 0;
    const fs::path androidJar = findLatestAndroidJarInRoots(jarRoots, jarSdkRoot, targetApi);
    if (androidJar.empty()) {
        error = "No Android platform android.jar under " + sdkRoot.string() +
                " or " + localSdkRoot.string() +
                " (install one with: sdkmanager --sdk_root=" +
                localSdkRoot.string() + " \"platforms;android-34\").";
        return 2;
    }
    AndroidBuildTools buildTools = findLatestAndroidBuildTools(sdkRoot);
    if (buildTools.aapt2.empty() || buildTools.zipalign.empty() || buildTools.apksigner.empty()) {
        error = "Android SDK build-tools missing aapt2, zipalign, or apksigner.";
        return 2;
    }

    const fs::path outputDir = request.outputDir.empty()
        ? ModularityAndroidOutputDir(sourceRoot)
        : fs::path(request.outputDir);
    fs::create_directories(outputDir, ec);
    const fs::path buildRoot = outputDir / ("_android_build_" + abi);
    const fs::path apkStageRoot = outputDir / "apk";
    fs::remove_all(apkStageRoot, ec);
    ec.clear();
    const fs::path assetsRoot = apkStageRoot / "assets";
    const fs::path libDir = apkStageRoot / "lib" / abi;
    if (!stageAndroidCxxRuntime(ndkRoot, abi, libDir, error)) return 1;
    fs::create_directories(assetsRoot, ec);
    fs::create_directories(libDir, ec);
    fs::create_directories(buildRoot, ec);

    auto logcb = [](const std::string& s) { std::fprintf(stderr, "%s", s.c_str()); std::fflush(stderr); };
    const std::string config = request.debug ? "Debug" : "Release";

    std::string generatorArgs;
    if (!androidCMakeGeneratorArgs(sdkRoot, buildRoot, generatorArgs, error)) return 2;
    const fs::path embeddedRoot = request.editor ? findEmbeddedAndroidCompilers(sourceRoot) : fs::path{};
    if (!embeddedRoot.empty() && abi != "arm64-v8a") {
        error = "The embedded compiler SDK currently supports Android arm64-v8a.";
        return 2;
    }
    const std::string embeddedArgs = embeddedRoot.empty()
        ? " -DMODULARITY_ENABLE_EMBEDDED_COMPILERS=OFF"
        : " -DMODULARITY_ENABLE_EMBEDDED_COMPILERS=ON -DMODULARITY_EMBEDDED_COMPILERS_ROOT=" + quotePath(embeddedRoot);
    const std::string configureCmd =
        "cmake" + generatorArgs + " -S " + quotePath(sourceRoot) + " -B " + quotePath(buildRoot) + embeddedArgs +
        " -DCMAKE_BUILD_TYPE=" + config +
        " -DMODULARITY_BUILD_EDITOR=OFF"
        " -DCMAKE_TOOLCHAIN_FILE=" + quotePath(toolchainFile) +
        " -DANDROID_ABI=" + abi +
        " -DANDROID_PLATFORM=android-26"
        " -DANDROID_STL=c++_shared"
        " -DMODULARITY_ENABLE_ASSIMP=OFF";
    const bool editor = request.editor;
    const std::string cmakeTarget = editor ? "ModularityEditorAndroid" : "core_player";
    const std::string soName      = editor ? "libModularity.so" : "libModularityPlayer.so";
    const std::string libName     = editor ? "Modularity" : "ModularityPlayer";
    const std::string appLabel    = editor ? "Modularity Editor" : "Modularity Player";
    const std::string pkgSuffix   = editor ? "Editor" : "Player";
    const std::string apkStem     = editor ? "Modularity" : "ModularityPlayer";

    int rc = 0;
    std::fprintf(stderr, "[Android] Configuring %s build (abi=%s)...\n",
                 editor ? "editor" : "bare player", abi.c_str());
    if (!runCommandStreaming(configureCmd + " 2>&1", logcb, &rc)) {
        error = "Android CMake configure failed (exit code " + std::to_string(rc) + ").";
        return 1;
    }
    const std::string buildCmd =
        "cmake --build " + quotePath(buildRoot) + " --config " + config + " --target " + cmakeTarget + androidBuildParallelArgs();
    std::fprintf(stderr, "[Android] Building %s...\n", soName.c_str());
    if (!runCommandStreaming(buildCmd + " 2>&1", logcb, &rc)) {
        error = "Android " + std::string(editor ? "editor" : "player") +
                " build failed (exit code " + std::to_string(rc) + ").";
        return 1;
    }

    const fs::path builtSo = findAndroidSharedLibrary(buildRoot, soName);
    if (builtSo.empty()) {
        error = "Built Android library not found: " + soName;
        return 1;
    }
    const fs::path stagedSo = libDir / soName;
    fs::copy_file(builtSo, stagedSo, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Failed to stage Android library: " + ec.message();
        return 1;
    }
    const fs::path stripTool = findAndroidLlvmStrip(ndkRoot);
    if (!stripTool.empty()) {
        int stripRc = 0;
        runCommandStreaming(quotePath(stripTool) + " --strip-unneeded " + quotePath(stagedSo) + " 2>&1",
                            logcb, &stripRc);
    }

    if (editor) {
        const fs::path resSrc = sourceRoot / "Resources";
        if (fs::is_directory(resSrc, ec)) {
            std::error_code copyEc;
            fs::copy(resSrc, assetsRoot / "Resources",
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
            if (copyEc) {
                std::fprintf(stderr, "[Android] Warning: failed to bundle Resources: %s\n",
                             copyEc.message().c_str());
            } else {
                std::fprintf(stderr, "[Android] Bundled engine Resources into APK assets.\n");
            }
        }

        if (!embeddedRoot.empty()) {
            if (!stageEmbeddedAndroidCompilers(embeddedRoot, libDir, error) ||
                !bundleAndroidToolchain(embeddedRoot / "bundle", assetsRoot, abi, logcb, error, sourceRoot)) return 1;
            std::fprintf(stderr, "[Android] Embedded Cling/TinyCC runtime and script SDK bundled.\n");
        } else if (const char* clangDirEnv = std::getenv("MODULARITY_ANDROID_CLANG_DIR");
            clangDirEnv && *clangDirEnv) {
            std::string bundleErr;
            if (!bundleAndroidToolchain(fs::path(clangDirEnv), assetsRoot, abi, logcb, bundleErr)) {
                std::fprintf(stderr, "[Android] Warning: on-device compiler not bundled: %s\n",
                             bundleErr.c_str());
            }
        } else {
            std::fprintf(stderr, "[Android] Note: MODULARITY_ANDROID_CLANG_DIR not set; the "
                                 "editor APK will ship without an on-device compiler.\n");
        }

        std::string bridgeErr;
        if (!writeAndroidActivityBridge(apkStageRoot / "java", bridgeErr)) {
            error = bridgeErr;
            return 1;
        }
    }

    std::string iconRef;
    std::string perr;
    if (!writeAndroidLauncherIcon(apkStageRoot / "res", sourceRoot, iconRef, perr)) {
        error = perr;
        return 1;
    }
    const std::string packageName = makeAndroidPackageName("Modularity", pkgSuffix);
    if (!writeAndroidManifest(apkStageRoot / "AndroidManifest.xml", packageName,
                              appLabel, "0.1.0", request.debug, iconRef, perr, libName,
                              editor ? 28 : 34,
                              editor ? kAndroidBridgeActivityName : "android.app.NativeActivity",
                              editor,
                              AndroidXrManifestOptions{},
                              true)) {  // runs any project, so any of them may want the mic
        error = perr;
        return 1;
    }

    fs::path apkPath = request.outputApk.empty()
        ? (outputDir / (apkStem + ".apk"))
        : fs::absolute(request.outputApk, ec);
    if (ec) apkPath = request.outputApk;
    fs::create_directories(apkPath.parent_path(), ec);
    const fs::path keystorePath = outputDir / "signing" / "debug.keystore";
    if (!packageAndroidApk(apkStageRoot, apkPath, androidJar, buildTools, keystorePath, logcb, perr)) {
        error = perr;
        return 1;
    }

    std::fprintf(stdout, "Android APK: %s\n", apkPath.string().c_str());
    return 0;
}

void Engine::createNewProject(const char* name, const char* location) try {
    fs::path basePath(location);
    std::error_code baseDirEc;
    fs::create_directories(basePath, baseDirEc);
    if (baseDirEc) {
        projectManager.errorMessage = "Could not create project location: " + baseDirEc.message();
        return;
    }

    Project newProject(name, basePath);
    if (!projectManager.preferences.defaultCompanyName.empty()) {
        newProject.playerSettings.companyName = projectManager.preferences.defaultCompanyName;
    }
    newProject.pipeline = ProjectPipelineFromUiIndex(projectManager.newProjectPipelineMode);
    {
        const Modularity::HardwareProfile::Tier tier =
            projectManager.preferences.effectiveTier();
        if (tier == Modularity::HardwareProfile::Tier::Low) {
            ApplyProjectQualityPreset(newProject.graphicsSettings, ProjectQualityPreset::Low);
            addConsoleMessage("New project started on the Low graphics preset to match this "
                              "machine. Change it in Project Settings > Graphics.",
                              ConsoleMessageType::Info);
        } else if (tier == Modularity::HardwareProfile::Tier::Balanced) {
            ApplyProjectQualityPreset(newProject.graphicsSettings, ProjectQualityPreset::Balanced);
        }
    }
    newProject.rendererBackend = (projectManager.newProjectRendererMode == 1)
        ? Modularity::GraphicsBackend::Vulkan
        : Modularity::GraphicsBackend::OpenGL;
#if !MODULARITY_HAS_VULKAN
    if (newProject.rendererBackend == Modularity::GraphicsBackend::Vulkan) {
        newProject.rendererBackend = Modularity::GraphicsBackend::OpenGL;
        addConsoleMessage("Vulkan was selected for new project, but this build does not include Vulkan. Using OpenGL.",
                          ConsoleMessageType::Warning);
    }
#endif
    if (newProject.create()) {
        const std::string presetId = projectManager.newProjectPresetId.empty()
            ? std::string("empty")
            : projectManager.newProjectPresetId;
        auto writeStarterScript = [&](const fs::path& relativePath,
                                      const std::string& className,
                                      const std::string& purpose) {
            fs::path path = newProject.projectPath / relativePath;
            if (fs::exists(path)) return;
            std::error_code scriptEc;
            fs::create_directories(path.parent_path(), scriptEc);
            std::ofstream script(path, std::ios::trunc);
            if (!script.is_open()) return;
            script << "add ModuCPP;\n";
            script << "add ModuEngine;\n\n";
            script << "public class " << className << " : ModuBehaviour {\n";
            script << "    void Begin() {\n";
            script << "        // TODO: " << purpose << "\n";
            script << "    }\n\n";
            script << "    void Update() {\n";
            script << "    }\n";
            script << "}\n";
        };

        if (projectManager.newProjectTemplatePath.empty()) {
            if (presetId == "2d-game") {
                newProject.pipeline = ProjectPipeline::Pipeline2D;
                newProject.physicsSettings.enable2DPhysics = true;
                newProject.physicsSettings.enable3DPhysics = false;
                newProject.playerSettings.defaultScene = "Main";
                newProject.playerSettings.startupWidth = 1280;
                newProject.playerSettings.startupHeight = 720;
                std::error_code spritesDirEc;
                fs::create_directories(newProject.assetsPath / "Sprites", spritesDirEc);
                if (spritesDirEc) {
                    addConsoleMessage("Could not create Sprites folder: " + spritesDirEc.message(),
                                      ConsoleMessageType::Warning);
                }
                writeStarterScript(fs::path("Assets") / "Scripts" / "Runtime" / "PlayerController2D.moducpp",
                                   "PlayerController2D",
                                   "move the player sprite here");
            } else if (presetId == "tool-app") {
                newProject.pipeline = ProjectPipeline::Pipeline2D;
                newProject.playerSettings.defaultScene = "Main";
                newProject.playerSettings.startupWidth = 1280;
                newProject.playerSettings.startupHeight = 800;
                newProject.playerSettings.cursorLocked = false;
                newProject.playerSettings.cursorVisible = true;
                std::error_code uiDirEc;
                fs::create_directories(newProject.assetsPath / "UI", uiDirEc);
                if (uiDirEc) {
                    addConsoleMessage("Could not create UI folder: " + uiDirEc.message(),
                                      ConsoleMessageType::Warning);
                }
                writeStarterScript(fs::path("Assets") / "Scripts" / "Runtime" / "AppController.moducpp",
                                   "AppController",
                                   "wire app state and UI actions here");
            } else if (presetId == "runtime-only") {
                newProject.playerSettings.defaultScene = "Main";
                newProject.playerSettings.cursorLocked = false;
                newProject.playerSettings.cursorVisible = true;
            }
            newProject.saveProjectFile();
        }

        if (!projectManager.newProjectTemplatePath.empty()) {
            std::string templateError;
            if (!applyTemplateProject(fs::path(projectManager.newProjectTemplatePath),
                                      newProject,
                                      name,
                                      newProject.pipeline,
                                      newProject.rendererBackend,
                                      templateError)) {
                projectManager.errorMessage = templateError;
                return;
            }
        }

        bool importedPackages = false;
        std::string importedFromProject;
        if (projectManager.newProjectImportLastPackages) {
            std::string importError;
            if (importInstalledPackagesFromRecent(projectManager,
                                                  newProject.projectPath,
                                                  importedFromProject,
                                                  importError)) {
                importedPackages = true;
            } else if (!importError.empty()) {
                addConsoleMessage("Installed package import skipped: " + importError,
                                  ConsoleMessageType::Warning);
            }
        }

        projectManager.currentProject = newProject;
        projectManager.addToRecentProjects(name,
                                          (newProject.projectPath / "project.modu").string());

        packageManager.setProjectRoot(projectManager.currentProject.projectPath);
        const std::vector<std::string> defaultPackages = installDefaultPackagesIntoProject();
        clampOptionalPackageState(false);

        if (!initRenderer()) {
            logToConsole("Error: Failed to initialize renderer!");
            return;
        }

        if (!physics->isReady() && !physics->init()) {
            addConsoleMessage("Warning: PhysX failed to initialize; physics disabled for this session", ConsoleMessageType::Warning);
        }

        sceneObjects.clear();
        clearSelection();
        nextObjectId = 0;

        if (projectManager.newProjectTemplatePath.empty()) {
            if (presetId == "2d-game") {
                addObject(ObjectType::Canvas, "Canvas");
                addObject(ObjectType::Sprite2D, "Player");
                addObject(ObjectType::Light2D, "Global Light");
            } else if (presetId == "tool-app") {
                addObject(ObjectType::Canvas, "Canvas");
                addObject(ObjectType::UIText, "Title Text");
                addObject(ObjectType::UIButton, "Primary Button");
            } else if (presetId == "runtime-only") {
                addObject(ObjectType::Camera, "Runtime Camera");
            } else if (presetId != "empty") {
                createPipelineDefaultSceneObjects();
            }
        } else {
            createPipelineDefaultSceneObjects();
        }

        fs::path contentRoot = projectManager.currentProject.usesNewLayout
            ? projectManager.currentProject.assetsPath
            : projectManager.currentProject.projectPath;
        fileBrowser.setProjectRoot(contentRoot);
        fileBrowser.currentPath = contentRoot;
        fileBrowser.needsRefresh = true;
        scriptEditorWindowsDirty = true;
        scriptEditorWindows.clear();
        scriptLastAutoCompileTime.clear();
        scriptAutoCompileCheckedSourceTime.clear();
        scriptAutoCompileBinaryCache.clear();
        scriptAutoCompileDiscoveredSources.clear();
        ++scriptAutoCompileScanGeneration;
        scriptAutoCompileConfigValid = false;
        scriptBinaryResolveCache.clear();
        scriptBuildConfigCache.clear();
        autoCompileQueue.clear();
        autoCompileQueued.clear();
        scriptAutoCompileLastCheck = 0.0;
        scriptAutoCompileLastDirectoryScan = 0.0;
        managedAutoCompileLastScan = 0.0;
        managedAutoCompileCachedProjectDir.clear();
        managedAutoCompileNewestSource = fs::file_time_type{};
        managedAutoCompileHasSource = false;
        managedAutoCompileCompiledSource = fs::file_time_type{};
        managedAutoCompileHasCompiled = false;

        showLauncher = false;
        firstFrame = true;

        addConsoleMessage("Created new project: " + std::string(name), ConsoleMessageType::Success);
        addConsoleMessage("Project location: " + newProject.projectPath.string(), ConsoleMessageType::Info);
        addConsoleMessage("Pipeline: " + std::string(ProjectPipelineLabel(projectManager.currentProject.pipeline)),
                          ConsoleMessageType::Info);
        if (importedPackages) {
            addConsoleMessage("Imported installed packages from: " + importedFromProject, ConsoleMessageType::Info);
        }
        if (!defaultPackages.empty()) {
            std::string joined;
            for (const auto& id : defaultPackages) {
                if (!joined.empty()) joined += ", ";
                joined += id;
            }
            addConsoleMessage("Added default ModuPAKs: " + joined, ConsoleMessageType::Info);
        }

        saveCurrentScene();
        loadBuildSettings();
        if (projectManager.newProjectTemplatePath.empty()) {
            if (presetId == "runtime-only") {
                buildSettings.runtimeOnly = true;
                buildSettings.includeEditor = false;
                buildSettings.leanRuntimeExport = true;
                buildSettings.shipScriptSdk = false;
                buildSettings.outputFolder = "Builds";
                saveBuildSettings();
            } else if (presetId == "2d-game" || presetId == "tool-app") {
                buildSettings.runtimeOnly = true;
                buildSettings.includeEditor = false;
                saveBuildSettings();
            }
        }
        applyProjectPipelineDefaults(true);
    } else {
        projectManager.errorMessage = "Failed to create project directory";
    }
} catch (const std::exception& e) {
    projectManager.errorMessage = std::string("Failed to create project: ") + e.what();
}
#pragma endregion

#pragma region Scene Management
void Engine::loadRecentScenes() {
    sceneObjects.clear();
    clearSelection();
    nextObjectId = 0;
    undoStack.clear();
    redoStack.clear();

    fs::path scenePath = projectManager.currentProject.getSceneFilePath(projectManager.currentProject.currentSceneName);
    if (fs::exists(scenePath)) {
        beginDeferredSceneLoad(projectManager.currentProject.currentSceneName);
        return;
    } else {
        addConsoleMessage("Default scene not found, starting with a new scene.", ConsoleMessageType::Info);
        createPipelineDefaultSceneObjects();
        initializeNewSceneSerializationState(projectManager.currentProject.getSceneFilePath(projectManager.currentProject.currentSceneName));
        recordState("sceneLoaded");
    }

    fs::path contentRoot = projectManager.currentProject.usesNewLayout
        ? projectManager.currentProject.assetsPath
        : projectManager.currentProject.projectPath;
    fileBrowser.setProjectRoot(contentRoot);
    fileBrowser.currentPath = contentRoot;
    fileBrowser.needsRefresh = true;
}

fs::path Engine::getProjectPreviewPath(const fs::path& projectPathOrFile) const {
    if (projectPathOrFile.empty()) return {};
    fs::path root = projectPathOrFile;
    if (root.extension() == ".modu") {
        root = root.parent_path();
    }
    return root / "ProjectUserSettings" / "ProjectPreview.png";
}

void Engine::saveProjectPreview() {
    if (!projectManager.currentProject.isLoaded || !rendererInitialized) return;
    int width = renderer.getWidth();
    int height = renderer.getHeight();
    if (width <= 0 || height <= 0) return;

    unsigned int texId = getActiveSceneTexture();
    if (!texId) return;

    fs::path previewPath = getProjectPreviewPath(projectManager.currentProject.projectPath);
    if (previewPath.empty()) return;
    fs::create_directories(previewPath.parent_path());

#if MODULARITY_OPENGL_ES
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    GLuint srcFbo = 0, dstFbo = 0, dstTex = 0;
    glGenFramebuffers(1, &srcFbo);
    glGenFramebuffers(1, &dstFbo);
    glGenTextures(1, &dstTex);

    glBindTexture(GL_TEXTURE_2D, dstTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texId, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);

    const bool readbackOk =
        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    std::vector<unsigned char> pixels;
    if (readbackOk) {
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        pixels.resize(static_cast<size_t>(width) * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glDeleteFramebuffers(1, &srcFbo);
    glDeleteFramebuffers(1, &dstFbo);
    glDeleteTextures(1, &dstTex);

    if (!readbackOk) {
        std::cerr << "Project preview readback FBO incomplete; skipping.\n";
        return;
    }
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    stbi_write_png(previewPath.string().c_str(), width, height, 4, pixels.data(),
                   static_cast<int>(rowBytes));
#else
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    glBindTexture(GL_TEXTURE_2D, texId);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    stbi_write_png(previewPath.string().c_str(), width, height, 4, pixels.data(),
                   static_cast<int>(rowBytes));
#endif
}

namespace {

fs::path MakeCompatibilityScenePath(const fs::path& scenesPath, const fs::path& sourcePath) {
    fs::path compatibilityDir = scenesPath / "Compatibility";
    fs::create_directories(compatibilityDir);

    fs::path filename = sourcePath.filename();
    fs::path candidate = compatibilityDir / filename;
    if (!fs::exists(candidate)) {
        return candidate;
    }

    const std::string stem = sourcePath.stem().string();
    const std::string ext = sourcePath.extension().string();
    int suffix = 1;
    do {
        candidate = compatibilityDir / fs::path(stem + "_" + std::to_string(suffix) + ext);
        ++suffix;
    } while (fs::exists(candidate));
    return candidate;
}

bool MoveSceneFileToCompatibility(const fs::path& scenesPath, const fs::path& sourcePath, fs::path& outMovedPath) {
    if (sourcePath.empty() || !fs::exists(sourcePath)) {
        outMovedPath.clear();
        return true;
    }

    outMovedPath = MakeCompatibilityScenePath(scenesPath, sourcePath);
    std::error_code renameError;
    fs::rename(sourcePath, outMovedPath, renameError);
    if (!renameError) {
        return true;
    }

    std::error_code copyError;
    fs::copy_file(sourcePath, outMovedPath, fs::copy_options::overwrite_existing, copyError);
    if (copyError) {
        return false;
    }

    std::error_code removeError;
    fs::remove(sourcePath, removeError);
    return !removeError;
}

} // namespace

void Engine::resetPendingSceneSaveRequest() {
    pendingSceneSaveRequest = PendingSceneSaveRequest{};
}

void Engine::initializeNewSceneSerializationState(const fs::path& sourcePath) {
    currentSceneSerialization.version = SceneSerializationInternal::kModularSceneFormatVersion;
    currentSceneSerialization.fileFormat = SceneSerializer::FileFormat::ModularNodes;
    currentSceneSerialization.loadedFromLegacyLayout = false;
    currentSceneSerialization.upgradedToModularLayout = false;
    currentSceneSerialization.sourcePath = sourcePath;
    legacySceneSaveChoice = LegacySceneSaveChoice::SaveModular;
}

bool Engine::executeSceneSave(const std::string& destinationSceneName,
                              SceneSerializer::SavePreference preference,
                              bool moveLegacySourceToCompatibility) {
    if (!projectManager.currentProject.isLoaded) return false;

    const std::string resolvedSceneName = destinationSceneName.empty()
        ? projectManager.currentProject.currentSceneName
        : destinationSceneName;
    const fs::path scenePath = projectManager.currentProject.getSceneFilePath(resolvedSceneName);

    fs::path movedLegacyPath;
    if (moveLegacySourceToCompatibility &&
        currentSceneSerialization.fileFormat == SceneSerializer::FileFormat::LegacyFlat) {
        if (!MoveSceneFileToCompatibility(projectManager.currentProject.scenesPath,
                                          currentSceneSerialization.sourcePath,
                                          movedLegacyPath)) {
            addConsoleMessage("Error: Failed to move legacy scene into compatibility folder.", ConsoleMessageType::Error);
            return false;
        }
    }

    SceneSerializer::SaveOptions options;
    options.preference = preference;
    options.metadata = &currentSceneSerialization;

    if (!saveDirtyMeshEditAssetForSceneSave()) {
        if (!movedLegacyPath.empty()) {
            addConsoleMessage("Compatibility backup preserved at: " + movedLegacyPath.string(),
                              ConsoleMessageType::Warning);
        }
        return false;
    }

    const float timeOfDay = getSceneTimeOfDay();
    if (!SceneSerializer::saveScene(scenePath,
                                    sceneObjects,
                                    nextObjectId,
                                    timeOfDay,
                                    getSceneSkyboxSettings(),
                                    options)) {
        addConsoleMessage("Error: Failed to save scene!", ConsoleMessageType::Error);
        if (!movedLegacyPath.empty()) {
            addConsoleMessage("Compatibility backup preserved at: " + movedLegacyPath.string(),
                              ConsoleMessageType::Warning);
        }
        return false;
    }

#if !MODULARITY_RUNTIME_ONLY
    {
        std::string assemblageError;
        if (!saveDirtyAssemblages(assemblageError) && !assemblageError.empty()) {
            addConsoleMessage("Failed to save an Assemblage asset: " + assemblageError,
                              ConsoleMessageType::Error);
        }
    }
#endif

    projectManager.currentProject.currentSceneName = resolvedSceneName;
    projectManager.currentProject.hasUnsavedChanges = false;
    projectManager.currentProject.saveProjectFile();
    saveProjectPreview();

    {
        std::error_code ec;
        fs::remove(fs::path(scenePath.string() + ".editor"), ec);
    }

    if (preference == SceneSerializer::SavePreference::ForceLegacyFlat) {
        legacySceneSaveChoice = LegacySceneSaveChoice::KeepLegacy;
    } else {
        legacySceneSaveChoice = LegacySceneSaveChoice::SaveModular;
    }

    addConsoleMessage("Saved scene: " + resolvedSceneName, ConsoleMessageType::Success);
    if (!movedLegacyPath.empty()) {
        addConsoleMessage("Moved legacy scene to compatibility folder: " + movedLegacyPath.string(),
                          ConsoleMessageType::Info);
    }
    return true;
}

void Engine::continuePendingScenePostAction() {
    const PendingSceneSaveRequest pending = pendingSceneSaveRequest;
    resetPendingSceneSaveRequest();

    switch (pending.postAction) {
        case PendingScenePostAction::LoadScene:
            performLoadScene(pending.postActionPayload);
            break;
        case PendingScenePostAction::CreateNewScene:
            performCreateNewScene(pending.postActionPayload);
            break;
        case PendingScenePostAction::CloseProject:
            performCloseProject();
            break;
        case PendingScenePostAction::None:
        default:
            break;
    }
}

bool Engine::requestSceneSave(const std::string& destinationSceneName,
                              PendingScenePostAction postAction,
                              const std::string& postActionPayload,
                              bool allowLegacyUpgradePrompt) {
    if (!projectManager.currentProject.isLoaded) return false;

    if (isPlaying) {
        if (playModeSaveChoice == PlayModeSaveChoice::Ask) {
            pendingSceneSaveRequest.active = true;
            pendingSceneSaveRequest.destinationSceneName = destinationSceneName.empty()
                ? projectManager.currentProject.currentSceneName
                : destinationSceneName;
            pendingSceneSaveRequest.postAction = postAction;
            pendingSceneSaveRequest.postActionPayload = postActionPayload;
            pendingSceneSaveRequest.allowLegacyUpgradePrompt = allowLegacyUpgradePrompt;
            playModeSaveDontAskAgain = false;
            showPlayModeSaveDialog = true;
            playModeSaveDialogOpened = false;
            return false;
        }
        if (playModeSaveChoice == PlayModeSaveChoice::ExitPlayMode) {
            togglePlayMode();
        }
    }

    return performSceneSaveRequest(destinationSceneName, postAction, postActionPayload,
                                   allowLegacyUpgradePrompt);
}

bool Engine::performSceneSaveRequest(const std::string& destinationSceneName,
                                     PendingScenePostAction postAction,
                                     const std::string& postActionPayload,
                                     bool allowLegacyUpgradePrompt) {
    if (!projectManager.currentProject.isLoaded) return false;

    const bool loadedLegacyScene = currentSceneSerialization.fileFormat == SceneSerializer::FileFormat::LegacyFlat;
    if (allowLegacyUpgradePrompt &&
        loadedLegacyScene &&
        legacySceneSaveChoice == LegacySceneSaveChoice::Ask) {
        pendingSceneSaveRequest.active = true;
        pendingSceneSaveRequest.destinationSceneName = destinationSceneName.empty()
            ? projectManager.currentProject.currentSceneName
            : destinationSceneName;
        pendingSceneSaveRequest.postAction = postAction;
        pendingSceneSaveRequest.postActionPayload = postActionPayload;
        showLegacySceneLayoutDialog = true;
        legacySceneLayoutDialogOpened = false;
        return false;
    }

    const SceneSerializer::SavePreference preference =
        (loadedLegacyScene &&
         (legacySceneSaveChoice == LegacySceneSaveChoice::KeepLegacy ||
          (!allowLegacyUpgradePrompt && legacySceneSaveChoice == LegacySceneSaveChoice::Ask)))
            ? SceneSerializer::SavePreference::ForceLegacyFlat
            : SceneSerializer::SavePreference::PreferModular;
    const bool moveLegacySourceToCompatibility =
        loadedLegacyScene && preference == SceneSerializer::SavePreference::PreferModular;

    if (!executeSceneSave(destinationSceneName, preference, moveLegacySourceToCompatibility)) {
        return false;
    }

    pendingSceneSaveRequest.postAction = postAction;
    pendingSceneSaveRequest.postActionPayload = postActionPayload;
    continuePendingScenePostAction();
    return true;
}

bool Engine::saveCurrentScene(bool allowLegacyUpgradePrompt) {
    return requestSceneSave(projectManager.currentProject.currentSceneName,
                            PendingScenePostAction::None,
                            "",
                            allowLegacyUpgradePrompt);
}

void Engine::performLoadScene(const std::string& sceneName) {
    if (!projectManager.currentProject.isLoaded) return;

    MODU_PROFILE_SCOPE("Engine::performLoadScene", ProfilerSampleCategory::Asset);
    const auto __lsTotal = std::chrono::steady_clock::now();
    const fs::path scenePath = projectManager.currentProject.getSceneFilePath(sceneName);
    int sceneVersion = 9;
    float loadedTimeOfDay = -1.0f;
    SkyboxSettings loadedSkyboxSettings;
    SceneSerializer::Metadata loadedMetadata;
    const auto __lsBeforeLoad = std::chrono::steady_clock::now();
    if (SceneSerializer::loadScene(scenePath,
                                   sceneObjects,
                                   nextObjectId,
                                   sceneVersion,
                                   &loadedTimeOfDay,
                                   &loadedSkyboxSettings,
                                   &loadedMetadata)) {
        const auto __lsAfterLoad = std::chrono::steady_clock::now();
        markRuntimeScriptBindingsDirty();
        const auto __lsAfterMark = std::chrono::steady_clock::now();
        initializeLocalTransformsFromWorld(sceneVersion);
        const auto __lsAfterXf = std::chrono::steady_clock::now();
        ResetParticleSystem2DRuntimes(sceneObjects);
        const auto __lsAfterPS = std::chrono::steady_clock::now();
        rebuildSkeletalBindings();
        const auto __lsAfterSkel = std::chrono::steady_clock::now();
        auto msL = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::fprintf(stderr,
            "[ModuTimer] loadScene: serializer=%.1f mark=%.1f xforms=%.1f particles=%.1f skel=%.1f total=%.1f  (%s)\n",
            msL(__lsBeforeLoad, __lsAfterLoad),
            msL(__lsAfterLoad, __lsAfterMark),
            msL(__lsAfterMark, __lsAfterXf),
            msL(__lsAfterXf, __lsAfterPS),
            msL(__lsAfterPS, __lsAfterSkel),
            msL(__lsTotal, __lsAfterSkel),
            sceneName.c_str());
        undoStack.clear();
        redoStack.clear();
        {
            const fs::path tintPath = fs::path(scenePath.string() + ".editor");
            if (std::ifstream tin(tintPath); tin.good()) {
                std::unordered_map<int, glm::vec4> legacyTints;
                std::string line;
                while (std::getline(tin, line)) {
                    if (line.empty() || line[0] == '#') continue;
                    int id;
                    float r, g, b, a;
                    if (sscanf(line.c_str(), "%d %f,%f,%f,%f", &id, &r, &g, &b, &a) == 5) {
                        legacyTints[id] = glm::vec4(r, g, b, a);
                    }
                }
                for (SceneObject& obj : sceneObjects) {
                    auto it = legacyTints.find(obj.id);
                    if (it != legacyTints.end() && obj.editorIconTint == glm::vec4(1.0f)) {
                        obj.editorIconTint = it->second;
                    }
                }
            }
        }
        syncAssemblageProjectRoot();
        currentSceneSerialization = loadedMetadata;
        legacySceneSaveChoice = loadedMetadata.fileFormat == SceneSerializer::FileFormat::LegacyFlat
            ? LegacySceneSaveChoice::Ask
            : LegacySceneSaveChoice::SaveModular;
        projectManager.currentProject.currentSceneName = sceneName;
        projectManager.currentProject.hasUnsavedChanges = false;
        projectManager.currentProject.saveProjectFile();
        clearSelection();
        recordState("sceneLoaded");
        addConsoleMessage("Loaded scene: " + sceneName, ConsoleMessageType::Success);
        applySceneSkyboxSettings(loadedSkyboxSettings);
        applySceneTimeOfDay(loadedTimeOfDay >= 0.0f ? loadedTimeOfDay : 0.5f);
    } else {
        for (const auto& [message, type] : DescribeUnreadableScene(sceneName, scenePath, false)) {
            addConsoleMessage(message, type);
        }
    }
}

void Engine::loadScene(const std::string& sceneName) {
    if (!projectManager.currentProject.isLoaded) return;

    if (projectManager.currentProject.hasUnsavedChanges) {
        requestSceneSave(projectManager.currentProject.currentSceneName,
                         PendingScenePostAction::LoadScene,
                         sceneName,
                         true);
        return;
    }

    performLoadScene(sceneName);
}

void Engine::performCreateNewScene(const std::string& sceneName) {
    if (!projectManager.currentProject.isLoaded || sceneName.empty()) return;

    sceneObjects.clear();
    markRuntimeScriptBindingsDirty();
    clearSelection();
    nextObjectId = 0;
    undoStack.clear();
    redoStack.clear();

    projectManager.currentProject.currentSceneName = sceneName;
    projectManager.currentProject.hasUnsavedChanges = true;
    applySceneSkyboxSettings(SkyboxSettings{});
    applySceneTimeOfDay(0.5f);

    createPipelineDefaultSceneObjects();
    initializeNewSceneSerializationState(projectManager.currentProject.getSceneFilePath(sceneName));
    saveCurrentScene(false);
    recordState("newScene");

    addConsoleMessage("Created new scene: " + sceneName, ConsoleMessageType::Success);
}

void Engine::createNewScene(const std::string& sceneName) {
    if (!projectManager.currentProject.isLoaded || sceneName.empty()) return;

    if (projectManager.currentProject.hasUnsavedChanges) {
        requestSceneSave(projectManager.currentProject.currentSceneName,
                         PendingScenePostAction::CreateNewScene,
                         sceneName,
                         true);
        return;
    }

    performCreateNewScene(sceneName);
}

void Engine::performCloseProject() {
    projectManager.currentProject = Project();
    sceneObjects.clear();
    clearSelection();
    scriptEditorWindows.clear();
    scriptEditorWindowsDirty = true;
    resetBuildSettings();
    showBuildSettings = false;
    playerMode = false;
    autoStartRequested = false;
    autoStartPlayerMode = false;
    showLauncher = true;
    showLegacySceneLayoutDialog = false;
    legacySceneLayoutDialogOpened = false;
    resetPendingSceneSaveRequest();
    initializeNewSceneSerializationState();
}
#pragma endregion

#pragma region Scene Objects
void Engine::addObject(ObjectType type, const std::string& baseName) {
    recordState("addObject");
    int id = nextObjectId++;
    std::string name = baseName + " " + std::to_string(id);
    SceneObject obj(name, ObjectType::Empty, id);
    ApplyObjectPreset(obj, type);
    obj.localPosition = obj.position;
    obj.localRotation = NormalizeEulerDegrees(obj.rotation);
    obj.localScale = obj.scale;
    obj.localInitialized = true;
    sceneObjects.push_back(obj);
    markRuntimeScriptBindingsDirty();
    setPrimarySelection(id);
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    logToConsole("Created: " + name);
}

void Engine::duplicateSelected() {
    auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(),
        [this](const SceneObject& obj) { return obj.id == selectedObjectId; });

    if (it != sceneObjects.end()) {
        recordState("duplicate");
        int id = nextObjectId++;
        SceneObject newObj(it->name + " (Copy)", ObjectType::Empty, id);
        newObj.type = it->type;
        newObj.position = it->position + glm::vec3(1.0f, 0.0f, 0.0f);
        newObj.rotation = it->rotation;
        newObj.scale = it->scale;
        newObj.hasRenderer = it->hasRenderer;
        newObj.renderType = it->renderType;
        newObj.hasLight = it->hasLight;
        newObj.hasLight2D = it->hasLight2D;
        newObj.hasCamera = it->hasCamera;
        newObj.hasPostFX = it->hasPostFX;
        newObj.hasUI = it->hasUI;
        newObj.hasShadowCaster2D = it->hasShadowCaster2D;
        newObj.meshPath = it->meshPath;
        newObj.meshId = it->meshId;
        newObj.meshSourceIndex = it->meshSourceIndex;
        newObj.material = it->material;
        newObj.materialPath = it->materialPath;
        newObj.albedoTexturePath = it->albedoTexturePath;
        newObj.overlayTexturePath = it->overlayTexturePath;
        newObj.normalMapPath = it->normalMapPath;
        newObj.shaderPackPath = it->shaderPackPath;
        newObj.vertexShaderPath = it->vertexShaderPath;
        newObj.fragmentShaderPath = it->fragmentShaderPath;
        newObj.useOverlay = it->useOverlay;
        newObj.light = it->light;
        newObj.light2D = it->light2D;
        newObj.shadowCaster2D = it->shadowCaster2D;
        newObj.camera = it->camera;
        newObj.postFx = it->postFx;
        newObj.hasRigidbody = it->hasRigidbody;
        newObj.rigidbody = it->rigidbody;
        newObj.hasRigidbody2D = it->hasRigidbody2D;
        newObj.rigidbody2D = it->rigidbody2D;
        newObj.hasCollider2D = it->hasCollider2D;
        newObj.collider2D = it->collider2D;
        newObj.hasParallaxLayer2D = it->hasParallaxLayer2D;
        newObj.parallaxLayer2D = it->parallaxLayer2D;
        newObj.hasCameraFollow2D = it->hasCameraFollow2D;
        newObj.cameraFollow2D = it->cameraFollow2D;
        newObj.hasCollider = it->hasCollider;
        newObj.collider = it->collider;
        newObj.hasPlayerController = it->hasPlayerController;
        newObj.playerController = it->playerController;
        newObj.localPosition = newObj.position;
        newObj.localRotation = NormalizeEulerDegrees(newObj.rotation);
        newObj.localScale = newObj.scale;
        newObj.localInitialized = true;
        newObj.hasAudioSource = it->hasAudioSource;
        newObj.audioSource = it->audioSource;
        newObj.hasVideoPlayer = it->hasVideoPlayer;
        newObj.videoPlayer = it->videoPlayer;
        newObj.hasReverbZone = it->hasReverbZone;
        newObj.reverbZone = it->reverbZone;
        newObj.hasGroundBakedType = it->hasGroundBakedType;
        newObj.groundBakedType = it->groundBakedType;
        newObj.hasObsticleObject = it->hasObsticleObject;
        newObj.obsticleObject = it->obsticleObject;
        newObj.hasAIAgent = it->hasAIAgent;
        newObj.aiAgent = it->aiAgent;
        newObj.hasAnimation = it->hasAnimation;
        newObj.animation = it->animation;
        newObj.hasSkeletalAnimation = it->hasSkeletalAnimation;
        newObj.skeletal = it->skeletal;
        newObj.hasRig25DRoot = it->hasRig25DRoot;
        newObj.rig25DRoot = it->rig25DRoot;
        newObj.hasRig25DNode = it->hasRig25DNode;
        newObj.rig25DNode = it->rig25DNode;
        if (newObj.hasRig25DNode) {
            newObj.rig25DNode.nodeId = AllocateNextRig25DNodeId(sceneObjects);
            newObj.rig25DNode.nodeName = newObj.name;
        }
        newObj.hasMapRoot = it->hasMapRoot;
        newObj.mapRoot = it->mapRoot;
        newObj.hasMapSector = it->hasMapSector;
        newObj.mapSector = it->mapSector;
        newObj.hasMapTransition = it->hasMapTransition;
        newObj.mapTransition = it->mapTransition;
        newObj.hasMapPortal = it->hasMapPortal;
        newObj.mapPortal = it->mapPortal;
        newObj.hasMapMesh = it->hasMapMesh;
        newObj.mapMesh = it->mapMesh;
        MapMaker::PrepareDuplicatedObjectMapComponents(newObj);
        newObj.ui = it->ui;

        sceneObjects.push_back(newObj);
        markRuntimeScriptBindingsDirty();
        setPrimarySelection(id);
        if (projectManager.currentProject.isLoaded) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        logToConsole("Duplicated: " + newObj.name);
    }
}

void Engine::copySelected() {
    std::vector<int> ids = selectedObjectIds;
    if (ids.empty() && selectedObjectId >= 0) {
        ids.push_back(selectedObjectId);
    }
    if (ids.empty()) {
        return;
    }

    std::unordered_set<int> idSet(ids.begin(), ids.end());
    objectClipboard.clear();
    objectClipboard.reserve(idSet.size());
    for (const auto& obj : sceneObjects) {
        if (idSet.count(obj.id) == 0) continue;
        SceneObject copy = obj;
        copy.childIds.erase(std::remove_if(copy.childIds.begin(), copy.childIds.end(),
                          [&idSet](int id) { return idSet.count(id) == 0; }),
                          copy.childIds.end());
        objectClipboard.push_back(std::move(copy));
    }

    addConsoleMessage("Copied " + std::to_string(objectClipboard.size()) + " object(s).", ConsoleMessageType::Info);
}

void Engine::pasteClipboard() {
    if (objectClipboard.empty()) {
        return;
    }

    recordState("paste");

    std::unordered_map<int, int> idMap;
    idMap.reserve(objectClipboard.size());
    std::vector<int> newIds;
    newIds.reserve(objectClipboard.size());
    std::vector<int> oldParents;
    oldParents.reserve(objectClipboard.size());

    for (const auto& tpl : objectClipboard) {
        SceneObject copy = tpl;
        const int oldId = copy.id;
        const int newId = nextObjectId++;
        idMap[oldId] = newId;
        oldParents.push_back(copy.parentId);

        copy.id = newId;
        copy.name += " (Copy)";
        copy.position = tpl.position + glm::vec3(1.0f, 0.0f, 0.0f);
        copy.parentId = -1;
        copy.childIds.clear();
        copy.localPosition = copy.position;
        copy.localRotation = NormalizeEulerDegrees(copy.rotation);
        copy.localScale = copy.scale;
        copy.localInitialized = true;
        if (copy.hasRig25DNode) {
            copy.rig25DNode.nodeId = AllocateNextRig25DNodeId(sceneObjects);
            copy.rig25DNode.nodeName = copy.name;
        }

        sceneObjects.push_back(std::move(copy));
        newIds.push_back(newId);
    }

    for (size_t i = 0; i < newIds.size(); ++i) {
        SceneObject* newObj = findObjectById(newIds[i]);
        if (!newObj) continue;

        auto linkedUi = idMap.find(newObj->ui.linkedObjectId);
        if (linkedUi != idMap.end()) newObj->ui.linkedObjectId = linkedUi->second;
        const int oldParentId = oldParents[i];
        int resolvedParent = -1;
        auto mapped = idMap.find(oldParentId);
        if (mapped != idMap.end()) {
            resolvedParent = mapped->second;
        } else if (findObjectById(oldParentId) != nullptr) {
            resolvedParent = oldParentId;
        }
        newObj->parentId = resolvedParent;

        if (resolvedParent != -1) {
            if (SceneObject* parent = findObjectById(resolvedParent)) {
                if (std::find(parent->childIds.begin(), parent->childIds.end(), newObj->id) == parent->childIds.end()) {
                    parent->childIds.push_back(newObj->id);
                }
            }
        }

        glm::vec3 parentPos(0.0f);
        glm::quat parentRot(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 parentScale(1.0f);
        if (newObj->parentId != -1) {
            if (SceneObject* parent = findObjectById(newObj->parentId)) {
                parentPos = parent->position;
                parentRot = QuatFromEulerXYZ(parent->rotation);
                parentScale = parent->scale;
            }
        }
        updateLocalFromWorld(*newObj, parentPos, parentRot, parentScale);
    }

    {
        std::vector<SceneObject*> pastedObjects;
        pastedObjects.reserve(newIds.size());
        for (int newId : newIds) {
            if (SceneObject* pasted = findObjectById(newId)) {
                pastedObjects.push_back(pasted);
            }
        }
        MapMaker::RemapMapIdsForPastedObjects(pastedObjects);
    }

    updateHierarchyWorldTransforms();
    markRuntimeScriptBindingsDirty();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    selectedObjectIds = newIds;
    selectedObjectId = selectedObjectIds.empty() ? -1 : selectedObjectIds.back();
    addConsoleMessage("Pasted " + std::to_string(newIds.size()) + " object(s).", ConsoleMessageType::Success);
}

void Engine::selectAllObjects() {
    selectedObjectIds.clear();
    selectedObjectIds.reserve(sceneObjects.size());
    for (const auto& obj : sceneObjects) {
        selectedObjectIds.push_back(obj.id);
    }
    selectedObjectId = selectedObjectIds.empty() ? -1 : selectedObjectIds.back();
}

void Engine::deleteSelected() {
    if (selectedObjectId < 0 && selectedObjectIds.empty()) {
        return;
    }

    recordState("delete");

    std::unordered_map<int, SceneObject*> idLookup;
    idLookup.reserve(sceneObjects.size());
    for (auto& obj : sceneObjects) {
        idLookup.emplace(obj.id, &obj);
    }

    std::unordered_set<int> toDelete;
    std::vector<int> stack;
    if (!selectedObjectIds.empty()) {
        for (int id : selectedObjectIds) {
            if (id >= 0 && toDelete.insert(id).second) {
                stack.push_back(id);
            }
        }
    } else if (selectedObjectId >= 0) {
        toDelete.insert(selectedObjectId);
        stack.push_back(selectedObjectId);
    }

    while (!stack.empty()) {
        int currentId = stack.back();
        stack.pop_back();
        auto it = idLookup.find(currentId);
        if (it == idLookup.end() || !it->second) continue;

        for (int childId : it->second->childIds) {
            if (childId >= 0 && toDelete.insert(childId).second) {
                stack.push_back(childId);
            }
        }

        for (const auto& obj : sceneObjects) {
            if (obj.parentId == currentId && toDelete.insert(obj.id).second) {
                stack.push_back(obj.id);
            }
        }
    }

    auto it = std::remove_if(sceneObjects.begin(), sceneObjects.end(),
        [&toDelete](const SceneObject& obj) { return toDelete.count(obj.id) > 0; });

    if (it != sceneObjects.end()) {
        sceneObjects.erase(it, sceneObjects.end());
        markRuntimeScriptBindingsDirty();
        for (auto& obj : sceneObjects) {
            if (toDelete.count(obj.parentId) > 0) {
                obj.parentId = -1;
            }
            obj.childIds.erase(std::remove_if(obj.childIds.begin(), obj.childIds.end(),
                [&toDelete](int id) { return toDelete.count(id) > 0; }), obj.childIds.end());
        }
        updateHierarchyWorldTransforms();
        logToConsole("Deleted object");
        clearSelection();
        if (projectManager.currentProject.isLoaded) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
    }
}

void Engine::setParent(int childId, int parentId, int beforeSiblingId) {
    recordState("reparent");
    auto childIt = std::find_if(sceneObjects.begin(), sceneObjects.end(),
        [childId](const SceneObject& obj) { return obj.id == childId; });

    if (childIt == sceneObjects.end()) return;
    if (parentId == childId) return;
    if (beforeSiblingId == childId) {
        if (parentId == childIt->parentId) {
            return;
        }
        beforeSiblingId = -1;
    }

    if (parentId != -1) {
        int current = parentId;
        std::unordered_set<int> visitedAncestors;
        visitedAncestors.reserve(sceneObjects.size());
        while (current != -1) {
            if (current == childId) return;
            if (!visitedAncestors.insert(current).second) return;
            SceneObject* ancestor = findObjectById(current);
            current = ancestor ? ancestor->parentId : -1;
        }
    }

    const int oldParentId = childIt->parentId;

    if (oldParentId != -1) {
        auto oldParentIt = std::find_if(sceneObjects.begin(), sceneObjects.end(),
            [oldParentId](const SceneObject& obj) { return obj.id == oldParentId; });
        if (oldParentIt != sceneObjects.end()) {
            auto& children = oldParentIt->childIds;
            children.erase(std::remove(children.begin(), children.end(), childId), children.end());
        }
    }

    int resolvedParentId = parentId;
    if (resolvedParentId != -1) {
        auto newParentIt = std::find_if(sceneObjects.begin(), sceneObjects.end(),
            [resolvedParentId](const SceneObject& obj) { return obj.id == resolvedParentId; });
        if (newParentIt == sceneObjects.end()) {
            resolvedParentId = -1;
        }
    }

    childIt = std::find_if(sceneObjects.begin(), sceneObjects.end(),
        [childId](const SceneObject& obj) { return obj.id == childId; });
    if (childIt == sceneObjects.end()) return;
    childIt->parentId = resolvedParentId;

    if (resolvedParentId != -1) {
        auto newParentIt = std::find_if(sceneObjects.begin(), sceneObjects.end(),
            [resolvedParentId](const SceneObject& obj) { return obj.id == resolvedParentId; });
        if (newParentIt != sceneObjects.end()) {
            auto& children = newParentIt->childIds;
            children.erase(std::remove(children.begin(), children.end(), childId), children.end());
            if (beforeSiblingId != -1) {
                auto beforeIt = std::find(children.begin(), children.end(), beforeSiblingId);
                if (beforeIt != children.end()) {
                    children.insert(beforeIt, childId);
                } else {
                    children.push_back(childId);
                }
            } else {
                children.push_back(childId);
            }
        }
    } else {
        auto currentIt = std::find_if(sceneObjects.begin(), sceneObjects.end(),
            [childId](const SceneObject& obj) { return obj.id == childId; });
        if (currentIt != sceneObjects.end()) {
            size_t insertIndex = sceneObjects.size();
            if (beforeSiblingId != -1) {
                auto beforeIt = std::find_if(sceneObjects.begin(), sceneObjects.end(),
                    [beforeSiblingId](const SceneObject& obj) { return obj.id == beforeSiblingId && obj.parentId == -1; });
                if (beforeIt != sceneObjects.end()) {
                    insertIndex = static_cast<size_t>(std::distance(sceneObjects.begin(), beforeIt));
                }
            }

            size_t oldIndex = static_cast<size_t>(std::distance(sceneObjects.begin(), currentIt));
            SceneObject moved = std::move(*currentIt);
            sceneObjects.erase(currentIt);
            if (oldIndex < insertIndex && insertIndex > 0) {
                --insertIndex;
            }
            if (insertIndex > sceneObjects.size()) insertIndex = sceneObjects.size();
            sceneObjects.insert(sceneObjects.begin() + static_cast<std::ptrdiff_t>(insertIndex), std::move(moved));
            sceneObjectIndexData = nullptr;
        }
    }
    {
        glm::vec3 parentPos(0.0f);
        glm::quat parentRot(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 parentScale(1.0f);
        if (resolvedParentId != -1) {
            if (SceneObject* parent = findObjectById(resolvedParentId)) {
                parentPos = parent->position;
                parentRot = QuatFromEulerXYZ(parent->rotation);
                parentScale = parent->scale;
            }
        }
        childIt = std::find_if(sceneObjects.begin(), sceneObjects.end(),
            [childId](const SceneObject& obj) { return obj.id == childId; });
        if (childIt == sceneObjects.end()) return;
        updateLocalFromWorld(*childIt, parentPos, parentRot, parentScale);
    }

    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
}
#pragma endregion

#pragma region Console Logging
void Engine::addConsoleMessage(const std::string& message, ConsoleMessageType type) {
    if (type == ConsoleMessageType::Error) {
        playEditorSound(EditorSoundCue::Error, 0.95f);
    }

    auto now = std::chrono::system_clock::now();
    std::time_t clockTime = std::chrono::system_clock::to_time_t(now);
    char timeStr[16] = "00:00:00";
    std::tm tmNow{};
#if defined(_WIN32)
    localtime_s(&tmNow, &clockTime);
#else
    localtime_r(&clockTime, &tmNow);
#endif
    std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmNow);

    ConsoleEntry entry;
    entry.timestamp = timeStr;
    entry.message = message;
    entry.type = type;
    consoleLog.push_back(std::move(entry));
    Modularity::CrashReporter::AppendLogLine(std::string("[") + timeStr + "] " + message);

    if (type == ConsoleMessageType::Error) {
        latestErrorMessage = message;
        latestErrorTimestamp = timeStr;
    }

    if (consoleLog.size() > 1000) {
        consoleLog.erase(consoleLog.begin());
    }

#if !MODULARITY_RUNTIME_ONLY
    if (!playerMode && projectManager.currentProject.isLoaded) {
        const ProjectConsoleSettings& settings = projectManager.currentProject.consoleSettings;
        const bool shouldOpen = settings.openOnlyOnErrors
            ? type == ConsoleMessageType::Error
            : true;
        if (shouldOpen) {
            showConsole = true;
            consolePanelExpanded = true;
        }
    }
#endif
}

void Engine::logToConsole(const std::string& message) {
    addConsoleMessage(message, ConsoleMessageType::Info);
}

void Engine::showEditorToast(const std::string& message, ConsoleMessageType type, double holdSeconds) {
    if (message.empty()) {
        editorToast.visible = false;
        editorToast.message.clear();
        return;
    }

    editorToast.visible = true;
    editorToast.type = type;
    editorToast.message = message;
    editorToast.startTime = glfwGetTime();
    editorToast.holdSeconds = std::max(0.2, holdSeconds);
    editorToast.progressMode = false;
    editorToast.sticky = false;
    editorToast.progress = 0.0f;
    editorToast.detail.clear();
}

void Engine::showEditorProgressToast(const std::string& title, const std::string& detail,
                                     float progress01, ConsoleMessageType type) {
    if (!editorToast.visible || !editorToast.progressMode) {
        editorToast.startTime = glfwGetTime();
    }
    editorToast.visible = true;
    editorToast.type = type;
    editorToast.message = title;
    editorToast.detail = detail;
    editorToast.progress = progress01;
    editorToast.progressMode = true;
    editorToast.sticky = true;
    editorToast.holdSeconds = 1.0;
}

void Engine::finishEditorProgressToast(const std::string& message, ConsoleMessageType type,
                                       double holdSeconds) {
    if (message.empty()) {
        editorToast.visible = false;
        editorToast.progressMode = false;
        editorToast.sticky = false;
        editorToast.message.clear();
        editorToast.detail.clear();
        return;
    }
    showEditorToast(message, type, holdSeconds);
}

void Engine::addConsoleMessageFromScript(const std::string& message, ConsoleMessageType type) {
    addConsoleMessage(message, type);
}

bool Engine::isRuntimeKeyDown(int key) const {
    return editorWindow && glfwGetKey(editorWindow, key) == GLFW_PRESS;
}

bool Engine::isRuntimeMouseDown(int button) const {
    return editorWindow && glfwGetMouseButton(editorWindow, button) == GLFW_PRESS;
}

glm::vec2 Engine::getRuntimeMouseDelta() const {
    ImGuiIO& io = ImGui::GetIO();
    glm::vec2 delta(io.MouseDelta.x, io.MouseDelta.y);
    if (glm::dot(delta, delta) > 1e-8f) {
        return delta;
    }
    if (!editorWindow) {
        return glm::vec2(0.0f);
    }

    struct CursorCache {
        int frame = -1;
        bool hasPos = false;
        double lastX = 0.0;
        double lastY = 0.0;
        glm::vec2 delta = glm::vec2(0.0f);
    };
    static std::unordered_map<const Engine*, CursorCache> cacheByEngine;
    CursorCache& cache = cacheByEngine[this];

    int frame = ImGui::GetFrameCount();
    if (cache.frame == frame) {
        return cache.delta;
    }

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(editorWindow, &x, &y);

    glm::vec2 computed(0.0f);
    if (cache.hasPos) {
        computed.x = static_cast<float>(x - cache.lastX);
        computed.y = static_cast<float>(y - cache.lastY);
    }

    cache.lastX = x;
    cache.lastY = y;
    cache.hasPos = true;
    cache.frame = frame;
    cache.delta = computed;
    return computed;
}
#pragma endregion

#pragma region Runtime Mouse Capture + Cursor
void Engine::requestRuntimeMouseCapture() {
    runtimeMouseCaptureRequestFrame = ImGui::GetFrameCount();
    runtimeMouseCaptureRequestTime = glfwGetTime();
}

bool Engine::runtimeWantsMouseCapture() const {
    if (!isPlaying || isPaused) return false;
    if (runtimePlayerControllerWantsMouse) return true;
    // A request holds for two frames or 0.1 s, so a script steering from a fixed-rate hook
    // (which skips render frames at high FPS) does not make the capture flicker.
    return ImGui::GetFrameCount() - runtimeMouseCaptureRequestFrame <= 2 ||
           glfwGetTime() - runtimeMouseCaptureRequestTime < 0.1;
}

void Engine::updateRuntimeMouseCapture() {
    if (!isPlaying) {
        runtimePlayerControllerWantsMouse = false;
        runtimeMouseCaptureRequestFrame = -1000;
        runtimeMouseCaptureRequestTime = -1000.0;
        runtimeMouseCaptureArmed = true;
        return;
    }

    // The built-in FPS PlayerController looks around with the mouse whenever it runs.
    bool playerControllerActive = false;
    for (const SceneObject& obj : sceneObjects) {
        if (obj.hasPlayerController && obj.playerController.enabled &&
            IsObjectEnabledInHierarchy(obj)) {
            playerControllerActive = true;
            break;
        }
    }
    runtimePlayerControllerWantsMouse = playerControllerActive;

    if (!runtimeWantsMouseCapture()) {
        // Nothing steers with the mouse any more (a menu opened, the look script was
        // disabled, play paused): hand the pointer back instead of keeping it hidden.
        gameViewCursorLocked = false;
        runtimeMouseCaptureArmed = true;
        return;
    }
    if (gameViewCursorLocked) {
        runtimeMouseCaptureArmed = false;
        return;
    }
    if (!runtimeMouseCaptureArmed) {
        return; // released with Escape: stays free until the player clicks back in
    }

    // Grab only where the player is already looking at the game - never from under an
    // editor panel, an unfocused window, the startup splash or a scene-view freelook.
    bool canGrab = false;
    if (playerMode) {
        const bool splashShowing =
            buildSettings.splashEnabled && buildSettings.splashDurationSeconds > 0.0f &&
            (startupSplashStartTime < 0.0 ||
             glfwGetTime() - startupSplashStartTime <
                 static_cast<double>(buildSettings.splashDurationSeconds));
        canGrab = editorWindow && glfwGetWindowAttrib(editorWindow, GLFW_FOCUSED) &&
                  !splashShowing;
    } else {
        GLFWwindow* gameWindow = runtimeCursorGameWindow ? runtimeCursorGameWindow : editorWindow;
        canGrab = showGameViewport && gameViewportFocused && !cursorLocked && gameWindow &&
                  glfwGetWindowAttrib(gameWindow, GLFW_FOCUSED);
    }
    if (canGrab) {
        gameViewCursorLocked = true;
        runtimeMouseCaptureArmed = false;
    }
}

void Engine::setRuntimeCursorImage(const std::string& path, float hotspotX, float hotspotY,
                                   float heightPx) {
    runtimeCursorImagePath = path;
    runtimeCursorHotspot = glm::clamp(glm::vec2(hotspotX, hotspotY), glm::vec2(0.0f), glm::vec2(1.0f));
    runtimeCursorHeightPx = std::clamp(heightPx, 8.0f, 256.0f);
}

void Engine::clearRuntimeCursorImage() {
    runtimeCursorImagePath.clear();
}

namespace {
// Area-weighted RGBA resample for cursor images. Colour is weighted by alpha so the
// transparent surround cannot bleed a dark fringe into a downscaled outline.
std::vector<unsigned char> ResampleCursorRgba(const unsigned char* src, int srcW, int srcH,
                                              int dstW, int dstH) {
    std::vector<unsigned char> dst(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4u, 0);
    const float stepX = static_cast<float>(srcW) / static_cast<float>(dstW);
    const float stepY = static_cast<float>(srcH) / static_cast<float>(dstH);
    for (int dy = 0; dy < dstH; ++dy) {
        const float y0 = static_cast<float>(dy) * stepY;
        const float y1 = y0 + stepY;
        const int yEnd = std::min(srcH, static_cast<int>(std::ceil(y1)));
        for (int dx = 0; dx < dstW; ++dx) {
            const float x0 = static_cast<float>(dx) * stepX;
            const float x1 = x0 + stepX;
            const int xEnd = std::min(srcW, static_cast<int>(std::ceil(x1)));
            float rgb[3] = {0.0f, 0.0f, 0.0f};
            float alpha = 0.0f;
            float area = 0.0f;
            for (int y = static_cast<int>(y0); y < yEnd; ++y) {
                const float wy = std::min(y1, y + 1.0f) - std::max(y0, static_cast<float>(y));
                if (wy <= 0.0f) continue;
                for (int x = static_cast<int>(x0); x < xEnd; ++x) {
                    const float wx = std::min(x1, x + 1.0f) - std::max(x0, static_cast<float>(x));
                    if (wx <= 0.0f) continue;
                    const unsigned char* p = src + (static_cast<size_t>(y) * srcW + x) * 4u;
                    const float weight = wx * wy;
                    const float a = (static_cast<float>(p[3]) / 255.0f) * weight;
                    rgb[0] += static_cast<float>(p[0]) * a;
                    rgb[1] += static_cast<float>(p[1]) * a;
                    rgb[2] += static_cast<float>(p[2]) * a;
                    alpha += a;
                    area += weight;
                }
            }
            unsigned char* out = dst.data() + (static_cast<size_t>(dy) * dstW + dx) * 4u;
            if (alpha > 1e-6f) {
                for (int c = 0; c < 3; ++c) {
                    out[c] = static_cast<unsigned char>(std::clamp(std::lround(rgb[c] / alpha), 0L, 255L));
                }
            }
            if (area > 0.0f) {
                out[3] = static_cast<unsigned char>(
                    std::clamp(std::lround(alpha / area * 255.0f), 0L, 255L));
            }
        }
    }
    return dst;
}
} // namespace

GLFWcursor* Engine::getRuntimeCursor(float contentScale) {
#if defined(__ANDROID__)
    (void)contentScale;
    return nullptr;
#else
    const int targetHeight = std::clamp(
        static_cast<int>(std::lround(runtimeCursorHeightPx * std::max(contentScale, 0.5f))), 8, 256);
    const std::string key = runtimeCursorImagePath + "|" + std::to_string(targetHeight) + "|" +
                            std::to_string(runtimeCursorHotspot.x) + "," +
                            std::to_string(runtimeCursorHotspot.y);
    auto found = runtimeCursorCache.find(key);
    if (found != runtimeCursorCache.end()) {
        return found->second; // nullptr: this one failed already, don't re-read it every frame
    }

    // Dragging an inspector slider mints a cursor per value; drop all but the live one.
    if (runtimeCursorCache.size() >= 16) {
        for (auto it = runtimeCursorCache.begin(); it != runtimeCursorCache.end();) {
            if (it->second && it->second == runtimeCursorApplied) {
                ++it;
                continue;
            }
            if (it->second) glfwDestroyCursor(it->second);
            it = runtimeCursorCache.erase(it);
        }
    }

    std::vector<uint8_t> bytes = Modularity::Platform::GetAssetSource().ReadAll(runtimeCursorImagePath);
    if (bytes.empty()) {
        const std::string resolved = resolveProjectAssetPath(runtimeCursorImagePath).string();
        if (!resolved.empty() && resolved != runtimeCursorImagePath) {
            bytes = Modularity::Platform::GetAssetSource().ReadAll(resolved);
        }
    }

    GLFWcursor* cursor = nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = nullptr;
    if (!bytes.empty()) {
        const StbFlipScope topDown(false); // GLFW cursor rows run top to bottom
        pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                       &width, &height, &channels, 4);
    }
    if (pixels && width > 0 && height > 0) {
        const int targetWidth = std::max(
            1, static_cast<int>(std::lround(static_cast<double>(width) * targetHeight / height)));
        std::vector<unsigned char> scaled =
            ResampleCursorRgba(pixels, width, height, targetWidth, targetHeight);
        GLFWimage image;
        image.width = targetWidth;
        image.height = targetHeight;
        image.pixels = scaled.data();
        const int hotX = std::clamp(
            static_cast<int>(std::lround(runtimeCursorHotspot.x * (targetWidth - 1))), 0, targetWidth - 1);
        const int hotY = std::clamp(
            static_cast<int>(std::lround(runtimeCursorHotspot.y * (targetHeight - 1))), 0, targetHeight - 1);
        cursor = glfwCreateCursor(&image, hotX, hotY);
    }
    if (pixels) {
        stbi_image_free(pixels);
    }
    if (!cursor) {
        addConsoleMessage("Cursor image could not be loaded: " + runtimeCursorImagePath,
                          ConsoleMessageType::Warning);
    }
    runtimeCursorCache[key] = cursor;
    return cursor;
#endif
}

void Engine::updateRuntimeCursorImage() {
    // The game/player viewport reports afresh each frame, so a hidden game view reports nothing.
    const bool overGame = playerMode || runtimeCursorOverGame;
    GLFWwindow* gameWindow = playerMode ? editorWindow : runtimeCursorGameWindow;
    runtimeCursorOverGame = false;
    runtimeCursorGameWindow = nullptr;
    if (!isPlaying) {
        runtimeCursorImagePath.clear();
    }

#if !defined(__ANDROID__)
    // A floating game view lives in its own OS window, which ImGui destroys when it docks
    // back - never hand GLFW a window that is no longer one of ImGui's viewports.
    auto windowAlive = [&](GLFWwindow* window) {
        if (!window) return false;
        if (window == editorWindow) return true;
        const ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        for (int i = 0; i < platformIo.Viewports.Size; ++i) {
            if (platformIo.Viewports[i]->PlatformHandle == window) return true;
        }
        return false;
    };

    GLFWcursor* wanted = nullptr;
    if (isPlaying && !gameViewCursorLocked && !runtimeCursorImagePath.empty() && overGame &&
        windowAlive(gameWindow)) {
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        glfwGetWindowContentScale(gameWindow, &scaleX, &scaleY);
        wanted = getRuntimeCursor(std::max(scaleX, scaleY));
    }

    ImGuiIO& io = ImGui::GetIO();
    if (wanted) {
        // Stop the ImGui backend putting its own arrow back over ours at the next NewFrame.
        if (!runtimeCursorOwnsImGuiFlag &&
            !(io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)) {
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            runtimeCursorOwnsImGuiFlag = true;
        }
        if (runtimeCursorApplied != wanted || runtimeCursorAppliedWindow != gameWindow) {
            if (runtimeCursorAppliedWindow && runtimeCursorAppliedWindow != gameWindow &&
                windowAlive(runtimeCursorAppliedWindow)) {
                glfwSetCursor(runtimeCursorAppliedWindow, nullptr);
            }
            glfwSetCursor(gameWindow, wanted);
            runtimeCursorApplied = wanted;
            runtimeCursorAppliedWindow = gameWindow;
        }
        return;
    }

    if (runtimeCursorApplied) {
        if (windowAlive(runtimeCursorAppliedWindow)) {
            glfwSetCursor(runtimeCursorAppliedWindow, nullptr);
        }
        runtimeCursorApplied = nullptr;
        runtimeCursorAppliedWindow = nullptr;
    }
    if (runtimeCursorOwnsImGuiFlag) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        runtimeCursorOwnsImGuiFlag = false;
    }
#endif
}
#pragma endregion

#pragma region Particle Systems
namespace {
// An asset's modification stamp for hot reload, or 0 when it is not a plain file on disk
// (inside a mounted bundle, or missing). 0 never equals a real stamp, so a file that
// appears later is still picked up.
long long ParticleAssetStamp(const fs::path& path) {
    if (path.empty()) return 0;
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<long long>(time.time_since_epoch().count());
}

// Reads an asset the way a built game will - through AssetSource, which sees inside a
// mounted .modbundle - falling back to the project-resolved path in the editor.
std::string ReadParticleAssetText(const std::string& path, const fs::path& resolved) {
    auto& source = Modularity::Platform::GetAssetSource();
    std::vector<uint8_t> bytes;
    if (source.Exists(path)) {
        bytes = source.ReadAll(path);
    } else if (!resolved.empty() && source.Exists(resolved.string())) {
        bytes = source.ReadAll(resolved.string());
    }
    return std::string(bytes.begin(), bytes.end());
}

void ClearParticleMaterial(ParticleSystem2DComponent& ps) {
    ps.runtimeMaterialTexture.clear();
    ps.runtimeMaterialTint = glm::vec4(1.0f);
    ps.runtimeMaterialEmission = glm::vec3(0.0f);
    ps.runtimeMaterialFilter = -1;
}
} // namespace

void Engine::updateParticleSystems(float dt) {
    const double now = glfwGetTime();
    for (SceneObject& obj : sceneObjects) {
        if (!obj.hasParticleSystem2D || !obj.particleSystem2D.enabled ||
            !IsObjectEnabledInHierarchy(obj)) {
            continue;
        }
        refreshParticleAssets(obj, now);
        ParticleRuntime::Simulate(obj, dt);
    }
}

void Engine::refreshParticleAssets(SceneObject& obj, double now) {
    ParticleSystem2DComponent& ps = obj.particleSystem2D;
    // In the editor a material or .spritesheet can be edited under a running preview, so the
    // files are re-stamped twice a second. A built game's assets cannot change; there each
    // is read once, when its path first appears.
    const bool restamp = !playerMode &&
                         (ps.runtimeAssetCheckTime < 0.0 || now - ps.runtimeAssetCheckTime >= 0.5);
    if (restamp) ps.runtimeAssetCheckTime = now;

    if (ps.materialPath.empty()) {
        if (!ps.runtimeMaterialKey.empty()) {
            ps.runtimeMaterialKey.clear();
            ps.runtimeMaterialStamp = 0;
            ClearParticleMaterial(ps);
        }
    } else {
        const fs::path resolved = resolveMaterialAssetPath(ps.materialPath);
        const bool keyChanged = ps.runtimeMaterialKey != ps.materialPath;
        const long long stamp =
            (keyChanged || restamp) ? ParticleAssetStamp(resolved) : ps.runtimeMaterialStamp;
        if (keyChanged || stamp != ps.runtimeMaterialStamp) {
            ps.runtimeMaterialKey = ps.materialPath;
            ps.runtimeMaterialStamp = stamp;
            MaterialFileData data;
            const std::string text = ReadParticleAssetText(ps.materialPath, resolved);
            if (!text.empty() && parseMaterialText(text, resolved.string(), data)) {
                ps.runtimeMaterialTexture = data.albedo;
                ps.runtimeMaterialTint = glm::vec4(data.props.color, data.props.alpha);
                ps.runtimeMaterialEmission = data.props.emissionColor * data.props.emissionStrength;
                ps.runtimeMaterialFilter = static_cast<int>(data.props.textureFilter);
            } else {
                ClearParticleMaterial(ps);
                addConsoleMessage("Particle System \"" + obj.name + "\": could not read material " +
                                      ps.materialPath,
                                  ConsoleMessageType::Warning);
            }
        }
    }

    // Copied: TexturePath can hand back a field this function is about to rewrite.
    const std::string texture = ParticleRuntime::TexturePath(obj);
    const bool wantsClips = ps.textureSheetEnabled &&
                            ps.textureSheetMode == ParticleSystem2DComponent::SheetSpriteClips &&
                            !texture.empty();
    if (!wantsClips) {
        if (!ps.runtimeSheetKey.empty() || !ps.runtimeSheetRects.empty()) {
            ps.runtimeSheetKey.clear();
            ps.runtimeSheetRects.clear();
            ps.runtimeSheetStamp = 0;
        }
        return;
    }
    // The same sidecar a UI Image reads its clips from: "<texture>.spritesheet".
    const std::string sidecar = texture + ".spritesheet";
    const fs::path resolvedSidecar(Modularity::Platform::ResolveAssetPath(sidecar));
    const bool keyChanged = ps.runtimeSheetKey != texture;
    const long long stamp =
        (keyChanged || restamp) ? ParticleAssetStamp(resolvedSidecar) : ps.runtimeSheetStamp;
    if (keyChanged || stamp != ps.runtimeSheetStamp) {
        ps.runtimeSheetKey = texture;
        ps.runtimeSheetStamp = stamp;
        ps.runtimeSheetRects.clear();
        const std::string text = ReadParticleAssetText(sidecar, resolvedSidecar);
        if (!text.empty()) {
            ps.runtimeSheetRects = ParseSpritesheet(text).document.rects;
        }
        if (ps.runtimeSheetRects.empty()) {
            addConsoleMessage("Particle System \"" + obj.name +
                                  "\": Sprite Clips needs a .spritesheet next to " + texture +
                                  " - showing the whole texture instead",
                              ConsoleMessageType::Warning);
        }
    }
}
#pragma endregion

#pragma region Object Lookup
SceneObject* Engine::findObjectByName(const std::string& name) {
    auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(), [&](const SceneObject& o) {
        return o.name == name;
    });
    if (it != sceneObjects.end()) return &(*it);
    return nullptr;
}

SceneObject* Engine::findObjectById(int id) {
    refreshSceneObjectIndexCache();
    auto it = sceneObjectIndexById.find(id);
    if (it == sceneObjectIndexById.end()) return nullptr;
    if (it->second >= sceneObjects.size()) return nullptr;
    return &sceneObjects[it->second];
}

bool Engine::propagateObjectRenameReferences(const std::string& oldName,
                                             const std::string& newName,
                                             int renamedObjectId) {
    (void)renamedObjectId;
    if (oldName.empty() || oldName == newName) return false;

    bool changed = false;
    for (SceneObject& obj : sceneObjects) {
        for (ScriptComponent& script : obj.scripts) {
            for (ScriptSetting& setting : script.settings) {
                if (RenameRewriteScriptSettingValue(setting.value, oldName, newName)) {
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return changed;
}
#pragma endregion

#pragma region Script Hooks
fs::path Engine::resolveScriptBinary(const fs::path& sourcePath) {
    ScriptBuildConfig config;
    std::string error;
    fs::path cfg = resolveScriptsConfigPath(projectManager.currentProject);
    std::error_code cfgEc;
    const fs::file_time_type configWriteTime = fs::exists(cfg, cfgEc) ? fs::last_write_time(cfg, cfgEc)
                                                                     : fs::file_time_type{};
    bool haveConfig = false;
    {
        const std::string configKey = cfg.lexically_normal().string();
        auto cacheIt = scriptBuildConfigCache.find(configKey);
        if (cacheIt != scriptBuildConfigCache.end() && cacheIt->second.valid &&
            cacheIt->second.configWriteTime == configWriteTime) {
            config = cacheIt->second.config;
            error = cacheIt->second.error;
            haveConfig = cacheIt->second.haveConfig;
        } else {
            haveConfig = scriptCompiler.loadConfig(cfg, config, error);
            ScriptBuildConfigCacheEntry entry;
            entry.configWriteTime = configWriteTime;
            entry.config = config;
            entry.haveConfig = haveConfig;
            entry.error = error;
            entry.valid = true;
            scriptBuildConfigCache[configKey] = std::move(entry);
        }
    }
#ifdef __ANDROID__
    if (haveConfig && !config.outDir.empty()) {
        scriptRuntime.setCompiledScriptsRoot(config.outDir);
    }
#endif
    if (haveConfig) {
        auto resolveSource = [&](const fs::path& input) -> fs::path {
            if (input.empty()) return {};
            std::error_code ec;
            fs::path abs = fs::absolute(input, ec);
            if (ec) abs = input;
            if (fs::exists(abs)) return abs;

            const fs::path rescued = Modularity::Platform::ResolveAssetPath(input.string());
            if (!rescued.empty() && rescued != input && fs::exists(rescued, ec) && !ec) {
                return rescued;
            }
            ec.clear();

            fs::path scriptsDir = config.scriptsDir;
            if (!scriptsDir.is_absolute()) {
                scriptsDir = projectManager.currentProject.projectPath / scriptsDir;
            }

            if (input.is_relative()) {
                fs::path candidate = projectManager.currentProject.projectPath / input;
                if (fs::exists(candidate)) return candidate;
            }

            auto remapSuffix = [&](const fs::path& marker) -> fs::path {
                std::vector<fs::path> parts;
                for (const auto& p : abs) parts.push_back(p);
                std::vector<fs::path> markerParts;
                for (const auto& p : marker) markerParts.push_back(p);
                if (markerParts.empty()) return {};
                for (size_t i = 0; i + markerParts.size() <= parts.size(); ++i) {
                    bool match = true;
                    for (size_t k = 0; k < markerParts.size(); ++k) {
                        if (parts[i + k] != markerParts[k]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        fs::path suffix;
                        for (size_t j = i + markerParts.size(); j < parts.size(); ++j) {
                            suffix /= parts[j];
                        }
                        if (!suffix.empty()) {
                            fs::path candidate = scriptsDir / suffix;
                            if (fs::exists(candidate)) return candidate;
                        }
                        break;
                    }
                }
                return {};
            };

            fs::path remapped = remapSuffix(fs::path("Assets") / "Scripts");
            if (!remapped.empty()) return remapped;
            remapped = remapSuffix("Scripts");
            if (!remapped.empty()) return remapped;

            if (!abs.filename().empty()) {
                fs::path candidate = scriptsDir / abs.filename();
                if (fs::exists(candidate)) return candidate;
            }
            return {};
        };

        fs::path resolvedSource = resolveSource(sourcePath);
        if (!resolvedSource.empty()) {
            std::error_code sourceEc;
            fs::path sourceAbs = fs::absolute(resolvedSource, sourceEc);
            if (sourceEc) sourceAbs = resolvedSource;
            sourceAbs = sourceAbs.lexically_normal();
            const std::string cacheKey = sourceAbs.string();
            const fs::file_time_type sourceWriteTime = fs::last_write_time(sourceAbs, sourceEc);
            if (!sourceEc) {
                auto cacheIt = scriptBinaryResolveCache.find(cacheKey);
                if (cacheIt != scriptBinaryResolveCache.end() &&
                    cacheIt->second.valid &&
                    cacheIt->second.sourceWriteTime == sourceWriteTime &&
                    cacheIt->second.configWriteTime == configWriteTime) {
                    std::error_code binEc;
                    if (!cacheIt->second.binaryPath.empty() &&
                        fs::exists(cacheIt->second.binaryPath, binEc) && !binEc) {
                        return cacheIt->second.binaryPath;
                    }
                    scriptBinaryResolveCache.erase(cacheIt);
                }

                std::error_code relEc;
                fs::path relToScripts = fs::relative(sourceAbs, config.scriptsDir, relEc);
                if (relEc) relToScripts.clear();
                bool hasDotDot = false;
                for (const auto& part : relToScripts) {
                    if (part == "..") {
                        hasDotDot = true;
                        break;
                    }
                }
                if (relToScripts.empty() || relToScripts.is_absolute() || hasDotDot) {
                    relToScripts.clear();
                }

                fs::path relativeParent = relToScripts.has_parent_path() ? relToScripts.parent_path() : fs::path();
                fs::path binaryPath = config.outDir / relativeParent;
                const std::string artifactStem = NativeScriptArtifactStem(sourceAbs);
#ifdef _WIN32
                binaryPath /= artifactStem + ".dll";
#else
                binaryPath /= artifactStem + ".so";
#endif
                std::error_code binEc;
                if (!binaryPath.empty() && fs::exists(binaryPath, binEc) && !binEc) {
                    ScriptBinaryResolveCacheEntry entry;
                    entry.sourceWriteTime = sourceWriteTime;
                    entry.configWriteTime = configWriteTime;
                    entry.binaryPath = binaryPath;
                    entry.valid = true;
                    scriptBinaryResolveCache[cacheKey] = std::move(entry);
                    return binaryPath;
                }
            }
        }
    }

    std::vector<fs::path> searchDirs;
    if (haveConfig) {
        fs::path compiledDir = config.outDir;
        if (!compiledDir.is_absolute()) {
            compiledDir = projectManager.currentProject.projectPath / compiledDir;
        }
        searchDirs.push_back(compiledDir);
    }
    searchDirs.push_back(projectManager.currentProject.projectPath / "Cache" / "ScriptBin");
    searchDirs.push_back(projectManager.currentProject.projectPath / "Library" / "CompiledScripts");

    std::unordered_set<std::string> seenDirs;
    std::string stem = NativeScriptArtifactStem(sourcePath);
    if (!stem.empty()) {
        for (const auto& dir : searchDirs) {
            std::error_code absEc;
            fs::path absDir = fs::absolute(dir, absEc);
            if (absEc) absDir = dir;
            std::string dirKey = absDir.lexically_normal().string();
            if (!seenDirs.insert(dirKey).second) continue;
            if (!fs::exists(absDir)) continue;

            std::error_code dirEc;
            for (auto it = fs::recursive_directory_iterator(absDir, dirEc);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (it->is_directory()) {
                    const std::string dirName = it->path().filename().string();
                    if (dirName == ".loaded" || dirName == ".staging") {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                fs::path p = it->path();
#ifdef _WIN32
                if (p.stem() == stem && p.extension() == ".dll") return p;
#else
                if (p.stem() == stem && p.extension() == ".so") return p;
#endif
            }
        }
    }

    return {};
}

fs::path Engine::resolveManagedAssembly(const fs::path& sourcePath) {
    if (sourcePath.empty()) return {};
    std::error_code ec;
    std::string ext = sourcePath.extension().string();
    if (ext == ".cs" || ext == ".csproj") {
        fs::path output = getManagedOutputDll();
        if (fs::exists(output)) return output;
    }
    fs::path abs = fs::absolute(sourcePath, ec);
    if (!ec && fs::exists(abs)) return abs;
    fs::path candidate = projectManager.currentProject.projectPath / sourcePath;
    if (fs::exists(candidate)) return candidate;
    return {};
}

fs::path Engine::getManagedProjectPath() const {
    fs::path root;
    if (projectManager.currentProject.isLoaded) {
        root = findManagedProjectRoot(projectManager.currentProject.projectPath);
    }
    if (root.empty()) {
        root = findManagedProjectRoot(fs::current_path());
    }
#if defined(__linux__)
    if (root.empty()) {
        std::error_code ec;
        fs::path exe = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            root = findManagedProjectRoot(exe.parent_path());
        }
    }
#endif
    if (root.empty()) {
        return fs::current_path() / "Scripts" / "Managed" / "ModuCPP.csproj";
    }
    return root / "Scripts" / "Managed" / "ModuCPP.csproj";
}

fs::path Engine::getManagedOutputDll() const {
    return managedOutputPathFromProject(getManagedProjectPath());
}

bool Engine::createScriptAsset(ScriptScaffoldKind kind,
                               const std::string& requestedName,
                               const fs::path& preferredDirectory,
                               fs::path& outPath,
                               ScriptLanguage& outLanguage,
                               std::string& outManagedType,
                               std::string& error) {
    outPath.clear();
    outLanguage = ScriptLanguage::Cpp;
    outManagedType.clear();
    error.clear();

    if (!projectManager.currentProject.isLoaded) {
        error = "no project is loaded";
        return false;
    }

    const fs::path projectRoot = projectManager.currentProject.projectPath;
    const std::string baseName = BuildScriptIdentifier(requestedName);

    fs::path targetDir;
    std::string extension;
    switch (kind) {
        case ScriptScaffoldKind::ModuMako:
            outLanguage = ScriptLanguage::Cpp;
            extension = ".modumako";
            break;
        case ScriptScaffoldKind::ModuCpp:
            outLanguage = ScriptLanguage::Cpp;
            extension = ".moducpp";
            break;
        case ScriptScaffoldKind::Cpp:
            outLanguage = ScriptLanguage::Cpp;
            extension = ".cpp";
            break;
        case ScriptScaffoldKind::C:
            outLanguage = ScriptLanguage::C;
            extension = ".c";
            break;
        case ScriptScaffoldKind::CSharp:
            outLanguage = ScriptLanguage::CSharp;
            extension = ".cs";
            targetDir = projectRoot / "Scripts" / "Managed";
            break;
    }

    if (outLanguage != ScriptLanguage::CSharp) {
        if (!preferredDirectory.empty()) {
            targetDir = preferredDirectory;
            if (!targetDir.is_absolute()) {
                targetDir = projectRoot / targetDir;
            }
        }
    }

    if (outLanguage != ScriptLanguage::CSharp) {
        if (targetDir.empty()) {
            ScriptBuildConfig config;
            std::string configError;
            const fs::path cfgPath = resolveScriptsConfigPath(projectManager.currentProject);
            if (scriptCompiler.loadConfig(cfgPath, config, configError) && !config.scriptsDir.empty()) {
                targetDir = config.scriptsDir;
                if (!targetDir.is_absolute()) {
                    targetDir = projectRoot / targetDir;
                }
            } else {
                targetDir = projectRoot / "Assets" / "Scripts";
            }
        }
    } else {
        fs::path engineRoot = findManagedProjectRoot(projectRoot);
        if (engineRoot.empty()) {
            engineRoot = findManagedProjectRoot(fs::current_path());
        }
#if defined(__linux__)
        if (engineRoot.empty()) {
            std::error_code exeEc;
            fs::path exe = fs::read_symlink("/proc/self/exe", exeEc);
            if (!exeEc) {
                engineRoot = findManagedProjectRoot(exe.parent_path());
            }
        }
#elif defined(_WIN32)
        if (engineRoot.empty()) {
            wchar_t buffer[MAX_PATH];
            DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (len > 0) {
                engineRoot = findManagedProjectRoot(fs::path(buffer).parent_path());
            }
        }
#endif
        if (!ensureProjectManagedCsproj(projectRoot, engineRoot, error)) {
            return false;
        }
    }

    std::error_code dirEc;
    fs::create_directories(targetDir, dirEc);
    if (dirEc) {
        error = "unable to create script folder " + targetDir.string();
        return false;
    }

    const fs::path requestedPath = targetDir / (baseName + extension);
    const fs::path createdPath = MakeUniqueSiblingPath(requestedPath);
    const std::string className = BuildScriptIdentifier(createdPath.stem().string());
    const std::string contents = BuildScriptTemplateContents(kind, className);
    if (!WriteTextFile(createdPath, contents, error)) {
        return false;
    }

    outPath = createdPath.lexically_normal();
    if (outLanguage == ScriptLanguage::CSharp) {
        outManagedType = "ModuCPP." + className;
    }
    scriptingFilesDirty = true;
    return true;
}

void Engine::markProjectDirty() {
    // In prefab edit mode the scene IS the asset, so the same edit signal drives the
    // asset's unsaved marker. The project flag is left alone: the scene on disk has
    // not changed, and setting it would prompt to save a scene nobody edited.
    if (moduObjEditMode) {
        moduObjEditDirty = true;
        return;
    }
    projectManager.currentProject.hasUnsavedChanges = true;
}

namespace {
std::string httpPostScriptRequest(const std::string& url,
                                const std::string& contentType,
                                const std::string& body,
                                const std::string& headers,
                                const fs::path& fallbackTempRoot,
                                bool* outSuccess = nullptr,
                                const std::function<bool(const std::string&)>& onChunk = {}) {
    if (outSuccess) *outSuccess = false;
#if defined(_WIN32)
    (void)fallbackTempRoot;
    return httpPostWindows(url, contentType, body, headers, onChunk, outSuccess);
#else
    std::error_code ec;
    fs::path tempRoot = fs::temp_directory_path(ec);
    if (ec || tempRoot.empty()) tempRoot = fallbackTempRoot;
    else tempRoot /= "ModularityScriptHttp";
    fs::create_directories(tempRoot, ec);
    if (ec) return "Failed to create HTTP temp directory: " + tempRoot.string();

    static std::atomic<uint64_t> serial{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string stem = "request_" + std::to_string(stamp) + "_" + std::to_string(serial++);
    const fs::path bodyPath = tempRoot / (stem + ".body");
    const fs::path headerPath = tempRoot / (stem + ".headers");
    {
        std::ofstream out(bodyPath, std::ios::binary | std::ios::trunc);
        if (!out) return "Failed to write HTTP request body: " + bodyPath.string();
        out << body;
        if (!out.good()) {
            out.close();
            fs::remove(bodyPath, ec);
            return "Failed to flush HTTP request body: " + bodyPath.string();
        }
    }

    std::ostringstream command;
    command << "curl -sS -L -X POST";
    if (onChunk) command << " -N";
    if (!contentType.empty()) command << " -H \"Content-Type: " << escapeShellArg(contentType) << "\"";
    for (const std::string& header : splitLines(headers)) {
        command << " -H \"" << escapeShellArg(header) << "\"";
    }
    command << " --dump-header \"" << escapeShellArg(headerPath.string()) << "\""
            << " --data-binary @\"" << escapeShellArg(bodyPath.string()) << "\""
            << " \"" << escapeShellArg(url) << "\" 2>&1";

    std::string output;
    bool cancelled = false;
    const bool transportOk = onChunk
        ? runCommandStreaming(command.str(), [&](const std::string& chunk) {
            output += chunk;
            if (!cancelled && !onChunk(chunk)) cancelled = true;
        }, nullptr)
        : runCommandCapture(command.str(), output);
    // Curl writes each redirect/proxy response header in order. Use the final
    // HTTP status independently of body text, including for streaming requests.
    int status = 0;
    std::ifstream responseHeaders(headerPath);
    std::string line;
    while (std::getline(responseHeaders, line)) {
        if (line.rfind("HTTP/", 0) == 0) {
            std::istringstream statusLine(line);
            std::string version;
            statusLine >> version >> status;
        }
    }
    responseHeaders.close();
    fs::remove(bodyPath, ec);
    fs::remove(headerPath, ec);
    if (outSuccess) *outSuccess = transportOk && !cancelled && status >= 200 && status < 400;
    if (!transportOk && output.empty()) return "curl POST failed: " + url;
    return output;
#endif
}

} // namespace

std::string Engine::httpPostFromScript(const std::string& url,
                                       const std::string& contentType,
                                       const std::string& body,
                                       const std::string& headers) {
    return httpPostScriptRequest(url, contentType, body, headers,
                                 resolveProjectPathFromScript("Library/Temp"));
}

int Engine::startHttpPostFromScript(const std::string& url,
                                    const std::string& contentType,
                                    const std::string& body,
                                    const std::string& headers,
                                    bool stream) {
    const int requestId = nextScriptHttpRequestId.fetch_add(1);
    auto state = std::make_shared<ScriptHttpRequestState>();
    state->id = requestId;
    state->stream = stream;

    {
        std::scoped_lock lock(scriptHttpRequestsMutex);
        scriptHttpRequests[requestId] = state;
    }

    // Resolve project-owned state on the caller thread. A detached request may
    // outlive the engine or a project switch; the worker must not access this.
    const fs::path fallbackTempRoot = resolveProjectPathFromScript("Library/Temp");
    std::thread([state, url, contentType, body, headers, fallbackTempRoot]() {
        auto pushChunk = [&](const std::string& chunk) -> bool {
            std::scoped_lock lock(state->mutex);
            if (state->cancelled) {
                return false;
            }
            if (!chunk.empty()) {
                state->pendingChunks.push_back(chunk);
            }
            return true;
        };

        bool success = false;
        const std::string response = httpPostScriptRequest(
            url, contentType, body, headers, fallbackTempRoot, &success,
            state->stream ? std::function<bool(const std::string&)>(pushChunk)
                          : std::function<bool(const std::string&)>{});

        {
            std::scoped_lock lock(state->mutex);
            state->bufferedResponse = response;
            if (!state->stream && !response.empty()) {
                state->pendingChunks.push_back(response);
            }
            state->success = success && !state->cancelled;
            state->done = true;
        }
    }).detach();

    return requestId;
}

bool Engine::pollHttpPostFromScript(int requestId, std::string& outChunk, bool& outDone, bool& outSuccess) {
    std::shared_ptr<ScriptHttpRequestState> state;
    {
        std::scoped_lock lock(scriptHttpRequestsMutex);
        auto it = scriptHttpRequests.find(requestId);
        if (it == scriptHttpRequests.end()) {
            outChunk.clear();
            outDone = true;
            outSuccess = false;
            return false;
        }
        state = it->second;
    }

    outChunk.clear();
    {
        std::scoped_lock lock(state->mutex);
        if (!state->pendingChunks.empty()) {
            outChunk = std::move(state->pendingChunks.front());
            state->pendingChunks.pop_front();
        }
        outDone = state->done && state->pendingChunks.empty();
        outSuccess = state->success;
    }

    if (outDone) {
        std::scoped_lock lock(scriptHttpRequestsMutex);
        scriptHttpRequests.erase(requestId);
    }

    return true;
}

void Engine::cancelHttpPostFromScript(int requestId) {
    std::scoped_lock lock(scriptHttpRequestsMutex);
    auto it = scriptHttpRequests.find(requestId);
    if (it == scriptHttpRequests.end()) {
        return;
    }
    std::scoped_lock stateLock(it->second->mutex);
    it->second->cancelled = true;
}

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <cerrno>
#endif

int Engine::startProcessFromScript(const std::string& command,
                                   const std::string& workingDir,
                                   bool interactive) {
    const int processId = nextScriptProcessId.fetch_add(1);
    auto state = std::make_shared<ScriptProcessState>();
    state->id = processId;

    std::string resolvedDir;
    if (!workingDir.empty()) {
        const fs::path p = resolveProjectPathFromScript(workingDir);
        if (!p.empty()) resolvedDir = p.string();
    }

    {
        std::scoped_lock lock(scriptProcessesMutex);
        scriptProcesses[processId] = state;
    }

#if defined(_WIN32)
    (void)interactive; // Windows stdin remains unsupported by this API.
    std::thread([state, command, resolvedDir]() {
        HANDLE readPipe = nullptr, writePipe = nullptr, nullInput = INVALID_HANDLE_VALUE;
        HANDLE job = nullptr;
        PROCESS_INFORMATION process{};
        auto closeHandle = [](HANDLE& handle) {
            if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            handle = nullptr;
        };
        auto fail = [&](const char* message) {
            const DWORD error = GetLastError();
            if (process.hProcess) TerminateProcess(process.hProcess, 1);
            closeHandle(process.hThread);
            closeHandle(process.hProcess);
            closeHandle(readPipe);
            closeHandle(writePipe);
            closeHandle(nullInput);
            closeHandle(job);
            std::scoped_lock lock(state->mutex);
            state->pendingChunks.push_back(std::string(message) + " (Windows error " + std::to_string(error) + ")\n");
            state->done = true;
        };
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&readPipe, &writePipe, &security, 0) ||
            !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
            fail("Failed to create process output pipe"); return;
        }
        nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &security, OPEN_EXISTING, 0, nullptr);
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (nullInput == INVALID_HANDLE_VALUE || !job ||
            !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            fail("Failed to configure process job"); return;
        }
        // Restrict inheritance to this worker's standard handles. Concurrent
        // processes must not keep each other's pipe writers open.
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<unsigned char> attributeStorage(attributeBytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
            fail("Failed to initialize process attributes"); return;
        }
        HANDLE inherited[] = {writePipe, nullInput};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inherited, sizeof(inherited), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(attributes);
            fail("Failed to configure inherited process handles"); return;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = writePipe;
        startup.StartupInfo.hStdError = writePipe;
        startup.StartupInfo.hStdInput = nullInput;
        startup.lpAttributeList = attributes;
        std::wstring commandLine = L"cmd.exe /D /S /C \"" + utf8ToWide(command) + L"\"";
        const std::wstring directory = utf8ToWide(resolvedDir);
        const bool created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process) != FALSE;
        const DWORD createError = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        if (!created) { SetLastError(createError); fail("Failed to start process"); return; }
        if (!AssignProcessToJobObject(job, process.hProcess)) {
            fail("Failed to assign process job"); return;
        }
        closeHandle(writePipe);
        closeHandle(nullInput);
        {
            std::scoped_lock lock(state->mutex);
            state->jobHandle = job;
            state->pid = process.dwProcessId;
            if (state->cancelled) TerminateJobObject(job, 1);
        }
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) TerminateJobObject(job, 1);
        closeHandle(process.hThread);
        char buffer[4096];
        DWORD bytes = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &bytes, nullptr) && bytes > 0) {
            std::scoped_lock lock(state->mutex);
            if (!state->cancelled) state->pendingChunks.emplace_back(buffer, bytes);
        }
        closeHandle(readPipe);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = static_cast<DWORD>(-1);
        GetExitCodeProcess(process.hProcess, &exitCode);
        closeHandle(process.hProcess);
        std::scoped_lock lock(state->mutex);
        state->jobHandle = nullptr;
        closeHandle(job);
        state->exitCode = static_cast<int>(exitCode);
        state->success = !state->cancelled && exitCode == 0;
        state->done = true;
    }).detach();
    return processId;
#else
    static std::atomic<bool> sigpipeIgnored{false};
    if (!sigpipeIgnored.exchange(true)) {
        signal(SIGPIPE, SIG_IGN);
    }

    auto failStart = [&](const char* message) {
        std::scoped_lock lock(state->mutex);
        state->pendingChunks.push_back(std::string(message) + "\n");
        state->done = true;
        state->exitCode = -1;
    };

    int outPipe[2] = {-1, -1};
    int inPipe[2]  = {-1, -1};
    if (pipe(outPipe) != 0) { failStart("Failed to create stdout pipe"); return processId; }
    if (interactive && pipe(inPipe) != 0) {
        ::close(outPipe[0]); ::close(outPipe[1]);
        failStart("Failed to create stdin pipe");
        return processId;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(outPipe[0]); ::close(outPipe[1]);
        if (interactive) { ::close(inPipe[0]); ::close(inPipe[1]); }
        failStart("fork() failed");
        return processId;
    }

    if (pid == 0) {
        if (interactive) dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(outPipe[1], STDERR_FILENO);
        ::close(outPipe[0]); ::close(outPipe[1]);
        if (interactive) { ::close(inPipe[0]); ::close(inPipe[1]); }
        if (!resolvedDir.empty()) { if (chdir(resolvedDir.c_str()) != 0) {  } }
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    ::close(outPipe[1]);
    const int readFd = outPipe[0];
    if (interactive) {
        ::close(inPipe[0]);
        std::scoped_lock lock(state->stdinMutex);
        state->stdinFd = inPipe[1];
    }
    {
        std::scoped_lock lock(state->mutex);
        state->pid = static_cast<long long>(pid);
    }

    std::thread([state, readFd, pid]() {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(readFd, buf, sizeof(buf));
            if (n > 0) {
                std::scoped_lock lock(state->mutex);
                if (!state->cancelled) state->pendingChunks.emplace_back(buf, static_cast<size_t>(n));
            } else if (n == 0) {
                break;
            } else {
                if (errno == EINTR) continue;
                break;
            }
        }
        ::close(readFd);
        int status = 0;
        waitpid(pid, &status, 0);
        const int code = WIFEXITED(status) ? WEXITSTATUS(status)
                       : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
        {
            std::scoped_lock lock(state->stdinMutex);
            if (state->stdinFd >= 0) { ::close(state->stdinFd); state->stdinFd = -1; }
        }
        std::scoped_lock lock(state->mutex);
        state->exitCode = code;
        state->success = (code == 0);
        state->done = true;
    }).detach();

    return processId;
#endif
}

bool Engine::writeProcessStdinFromScript(int processId, const std::string& data) {
    std::shared_ptr<ScriptProcessState> state;
    {
        std::scoped_lock lock(scriptProcessesMutex);
        auto it = scriptProcesses.find(processId);
        if (it == scriptProcesses.end()) return false;
        state = it->second;
    }
#if defined(_WIN32)
    (void)data;
    return false;
#else
    std::scoped_lock lock(state->stdinMutex);
    if (state->stdinFd < 0) return false;
    size_t total = 0;
    while (total < data.size()) {
        const ssize_t n = ::write(state->stdinFd, data.data() + total, data.size() - total);
        if (n > 0) { total += static_cast<size_t>(n); }
        else { if (errno == EINTR) continue; return false; }
    }
    return true;
#endif
}

bool Engine::pollProcessFromScript(int processId, std::string& outChunk, bool& outDone,
                                   bool& outSuccess, int& outExitCode) {
    std::shared_ptr<ScriptProcessState> state;
    {
        std::scoped_lock lock(scriptProcessesMutex);
        auto it = scriptProcesses.find(processId);
        if (it == scriptProcesses.end()) {
            outChunk.clear(); outDone = true; outSuccess = false; outExitCode = -1;
            return false;
        }
        state = it->second;
    }

    outChunk.clear();
    {
        std::scoped_lock lock(state->mutex);
        if (!state->pendingChunks.empty()) {
            outChunk = std::move(state->pendingChunks.front());
            state->pendingChunks.pop_front();
        }
        outDone = state->done && state->pendingChunks.empty();
        outSuccess = state->success;
        outExitCode = state->exitCode;
    }

    if (outDone) {
        std::scoped_lock lock(scriptProcessesMutex);
        scriptProcesses.erase(processId);
    }
    return true;
}

void Engine::cancelProcessFromScript(int processId) {
    std::shared_ptr<ScriptProcessState> state;
    {
        std::scoped_lock lock(scriptProcessesMutex);
        auto it = scriptProcesses.find(processId);
        if (it == scriptProcesses.end()) return;
        state = it->second;
    }
#if !defined(_WIN32)
    long long pid = -1;
    {
        std::scoped_lock lock(state->mutex);
        state->cancelled = true;
        pid = state->pid;
    }
    {
        std::scoped_lock lock(state->stdinMutex);
        if (state->stdinFd >= 0) { ::close(state->stdinFd); state->stdinFd = -1; }
    }
    if (pid > 0) kill(static_cast<pid_t>(pid), SIGKILL);
#else
    std::scoped_lock lock(state->mutex);
    state->cancelled = true;
    if (state->jobHandle) TerminateJobObject(static_cast<HANDLE>(state->jobHandle), 1);
#endif
}

std::string Engine::runProcessBlockingFromScript(const std::string& command,
                                                 const std::string& workingDir,
                                                 int timeoutMs, int* exitCode) {
    if (exitCode) *exitCode = -1;
    const int id = startProcessFromScript(command, workingDir, /*interactive=*/false);
    if (id <= 0) return "[process start failed]";

    std::string output;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        std::string chunk;
        bool done = false, success = false;
        int code = -1;
        if (!pollProcessFromScript(id, chunk, done, success, code)) { break; }
        if (!chunk.empty()) output += chunk;
        if (done) { if (exitCode) *exitCode = code; return output; }

        if (timeoutMs > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeoutMs) {
                cancelProcessFromScript(id);
                bool terminated = false;
                for (int i = 0; i < 400; ++i) {
                    std::string c2; bool d2 = false, s2 = false; int e2 = -1;
                    if (!pollProcessFromScript(id, c2, d2, s2, e2)) break;
                    if (!c2.empty()) output += c2;
                    if (d2) { code = e2; terminated = true; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (exitCode) *exitCode = code;
                return output + "\n[process timed out after " + std::to_string(timeoutMs) +
                       (terminated ? "ms; process terminated]" : "ms; cancellation requested, termination pending]");
            }
        }
        if (chunk.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return output;
}

bool Engine::readProcessLineFromScript(int processId, std::string& outLine, int timeoutMs) {
    outLine.clear();
    std::shared_ptr<ScriptProcessState> state;
    {
        std::scoped_lock lock(scriptProcessesMutex);
        auto it = scriptProcesses.find(processId);
        if (it == scriptProcesses.end()) return false;
        state = it->second;
    }

    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        bool done = false;
        {
            std::scoped_lock lock(state->mutex);
            while (!state->pendingChunks.empty()) {
                state->lineBuffer += std::move(state->pendingChunks.front());
                state->pendingChunks.pop_front();
            }
            done = state->done;
        }

        const size_t nl = state->lineBuffer.find('\n');
        if (nl != std::string::npos) {
            outLine = state->lineBuffer.substr(0, nl);
            state->lineBuffer.erase(0, nl + 1);
            if (!outLine.empty() && outLine.back() == '\r') outLine.pop_back();
            return true;
        }
        if (done) {
            if (!state->lineBuffer.empty()) { outLine.swap(state->lineBuffer); return true; }
            return false;
        }
        if (timeoutMs > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeoutMs) return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::string Engine::readFileTextFromScript(const std::string& path) const {
    const fs::path resolved = resolveProjectPathFromScript(path);
    if (resolved.empty()) {
        return {};
    }

    std::ifstream in(resolved, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }

    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

std::string Engine::readFileBase64FromScript(const std::string& path, size_t maxBytes) const {
    const fs::path resolved = resolveProjectPathFromScript(path);
    if (resolved.empty()) {
        return {};
    }

    std::error_code ec;
    const uintmax_t fileSize = fs::file_size(resolved, ec);
    if (ec || fileSize > static_cast<uintmax_t>(maxBytes)) {
        return {};
    }

    std::ifstream in(resolved, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }

    std::vector<unsigned char> bytes(static_cast<size_t>(fileSize));
    if (!bytes.empty()) {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!in) {
            return {};
        }
    }

    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned int b0 = bytes[i];
        const unsigned int b1 = (i + 1 < bytes.size()) ? bytes[i + 1] : 0;
        const unsigned int b2 = (i + 2 < bytes.size()) ? bytes[i + 2] : 0;
        const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < bytes.size()) ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < bytes.size()) ? kAlphabet[triple & 0x3F] : '=');
    }
    return out;
}

bool Engine::writeFileTextFromScript(const std::string& path, const std::string& content) {
    const fs::path resolved = resolveProjectPathFromScript(path);
    if (resolved.empty()) {
        return false;
    }

    std::error_code ec;
    fs::create_directories(resolved.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << content;
    if (!out.good()) {
        return false;
    }

    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return true;
}

bool Engine::deleteFileFromScript(const std::string& path) {
    const fs::path resolved = resolveProjectPathFromScript(path);
    if (resolved.empty()) {
        return false;
    }

    std::error_code ec;
    const bool removed = fs::remove(resolved, ec);
    if (!removed || ec) {
        return false;
    }

    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return true;
}

std::string Engine::listFilesFromScript(const std::string& path, bool recursive, int maxEntries) const {
    fs::path resolved = path.empty() ? getProgramRootPathFromScript() : fs::path(path);
    if (!resolved.is_absolute()) {
        resolved = resolveProjectPathFromScript(path);
    }
    if (resolved.empty()) {
        return {};
    }

    std::error_code ec;
    if (!fs::exists(resolved, ec) || ec) {
        return "Path not found: " + resolved.string();
    }

    std::vector<std::string> entries;
    maxEntries = std::max(maxEntries, 1);
    auto pushEntry = [&](const fs::path& p, bool isDirectory) {
        std::string line = p.lexically_normal().string();
        if (isDirectory) {
            line += "/";
        }
        entries.push_back(line);
    };

    if (fs::is_regular_file(resolved, ec) && !ec) {
        pushEntry(resolved, false);
    } else if (recursive) {
        for (auto it = fs::recursive_directory_iterator(resolved, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); ++it) {
            pushEntry(it->path(), it->is_directory());
            if (static_cast<int>(entries.size()) >= maxEntries) {
                break;
            }
        }
    } else {
        for (auto it = fs::directory_iterator(resolved, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); ++it) {
            pushEntry(it->path(), it->is_directory());
            if (static_cast<int>(entries.size()) >= maxEntries) {
                break;
            }
        }
    }

    std::sort(entries.begin(), entries.end());
    std::ostringstream out;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            out << '\n';
        }
        out << entries[i];
    }
    if (entries.empty()) {
        out << "(no files)";
    }
    return out.str();
}

std::string Engine::searchFilesFromScript(const std::string& root,
                                          const std::string& query,
                                          int maxResults) const {
    fs::path resolvedRoot = root.empty() ? getProgramRootPathFromScript() : fs::path(root);
    if (!resolvedRoot.is_absolute()) {
        resolvedRoot = resolveProjectPathFromScript(root);
    }
    if (resolvedRoot.empty()) {
        return {};
    }

    const std::string needleRaw = RenameTrim(query);
    if (needleRaw.empty()) {
        return {};
    }

    auto lowerCopy = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };

    const std::string needle = lowerCopy(needleRaw);
    maxResults = std::max(maxResults, 1);
    std::vector<std::string> results;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(resolvedRoot, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) {
            continue;
        }

        const fs::path filePath = it->path().lexically_normal();
        const std::string pathText = filePath.string();
        if (lowerCopy(pathText).find(needle) != std::string::npos) {
            results.push_back(pathText);
        } else {
            std::ifstream in(filePath, std::ios::binary);
            if (!in.is_open()) {
                continue;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            std::string content = ss.str();
            if (content.size() > 512 * 1024) {
                continue;
            }
            std::string lowered = lowerCopy(content);
            size_t found = lowered.find(needle);
            if (found != std::string::npos) {
                size_t start = content.rfind('\n', found);
                start = (start == std::string::npos) ? 0 : start + 1;
                size_t end = content.find('\n', found);
                if (end == std::string::npos) {
                    end = content.size();
                }
                std::string snippet = RenameTrim(content.substr(start, end - start));
                if (snippet.size() > 140) {
                    snippet = snippet.substr(0, 140) + "...";
                }
                results.push_back(pathText + " :: " + snippet);
            }
        }

        if (static_cast<int>(results.size()) >= maxResults) {
            break;
        }
    }

    std::ostringstream out;
    for (size_t i = 0; i < results.size(); ++i) {
        if (i > 0) {
            out << '\n';
        }
        out << results[i];
    }
    if (results.empty()) {
        out << "(no matches)";
    }
    return out.str();
}

ImTextureID Engine::getUIImageTextureFromScript(const std::string& path, int* outWidth, int* outHeight) {
    if (outWidth) *outWidth = 0;
    if (outHeight) *outHeight = 0;

    const fs::path resolved = resolveProjectPathFromScript(path);
    if (resolved.empty()) {
        return ImTextureID_Invalid;
    }

    const std::string resolvedPath = resolved.string();
    if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
        return vulkanRenderer->getOrCreateUIImage(resolvedPath, outWidth, outHeight);
    }

    if (Texture* tex = renderer.getTexture(resolvedPath, MaterialProperties::TextureFilter::Point);
        tex && tex->GetID() != 0) {
        if (outWidth) *outWidth = tex->GetWidth();
        if (outHeight) *outHeight = tex->GetHeight();
        return static_cast<ImTextureID>(tex->GetID());
    }

    return ImTextureID_Invalid;
}

std::string Engine::getSelectedFilePathFromScript() const {
    const fs::path& sel = fileBrowser.selectedFile;
    if (sel.empty()) {
        return {};
    }
    const Project& proj = projectManager.currentProject;
    if (proj.isLoaded && !proj.projectPath.empty()) {
        std::error_code ec;
        const fs::path rel = fs::relative(sel, proj.projectPath, ec);
        const std::string relStr = rel.generic_string();
        if (!ec && !relStr.empty() && relStr.rfind("..", 0) != 0) {
            return relStr;
        }
    }
    return sel.generic_string();
}

std::string Engine::getProjectNameFromScript() const {
    const Project& proj = projectManager.currentProject;
    if (!proj.name.empty()) {
        return proj.name;
    }
    if (!proj.projectPath.empty()) {
        return proj.projectPath.filename().generic_string();
    }
    return {};
}

std::string Engine::getCurrentSceneNameFromScript() const {
    return projectManager.currentProject.currentSceneName;
}

std::string Engine::getSelectedObjectInfoFromScript() const {
    if (selectedObjectId == -1) {
        return {};
    }
    const auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(),
        [this](const SceneObject& obj) { return obj.id == selectedObjectId; });
    if (it == sceneObjects.end()) {
        return {};
    }
    const SceneObject& o = *it;

    std::ostringstream out;
    out << "Selected object: \"" << o.name << "\" (id " << o.id << ")\n";
    out << "  enabled: " << (o.enabled ? "true" : "false")
        << ", tag: " << o.tag << ", layer: " << o.layer << "\n";
    out << "  position: (" << o.position.x << ", " << o.position.y << ", " << o.position.z << ")"
        << "  rotation: (" << o.rotation.x << ", " << o.rotation.y << ", " << o.rotation.z << ")"
        << "  scale: (" << o.scale.x << ", " << o.scale.y << ", " << o.scale.z << ")\n";

    std::vector<std::string> comps;
    if (o.hasRenderer) comps.push_back("Renderer");
    if (o.hasCamera) comps.push_back("Camera");
    if (o.hasLight) comps.push_back("Light");
    if (o.hasLight2D) comps.push_back("Light2D");
    if (o.hasRigidbody) comps.push_back("Rigidbody");
    if (o.hasRigidbody2D) comps.push_back("Rigidbody2D");
    if (o.hasCollider) comps.push_back("Collider");
    if (o.hasCollider2D) comps.push_back("Collider2D");
    if (o.hasParallaxLayer2D) comps.push_back("ParallaxLayer2D");
    if (o.hasCameraFollow2D) comps.push_back("CameraFollow2D");
    if (o.hasShadowCaster2D) comps.push_back("ShadowCaster2D");
    if (o.hasAudioSource) comps.push_back("AudioSource");
    if (o.hasPlayerController) comps.push_back("PlayerController");
    if (o.hasUI) comps.push_back("UI");
    if (!o.meshPath.empty()) {
        comps.push_back("Mesh(" + fs::path(o.meshPath).filename().generic_string() + ")");
    }
    out << "  components: ";
    if (comps.empty()) {
        out << "(none)";
    } else {
        for (size_t i = 0; i < comps.size(); ++i) {
            if (i > 0) out << ", ";
            out << comps[i];
        }
    }
    out << "\n";

    if (o.scripts.empty()) {
        out << "  scripts: (none)\n";
    } else {
        out << "  scripts:\n";
        for (const ScriptComponent& sc : o.scripts) {
            out << "    - " << (sc.path.empty() ? sc.managedType : sc.path);
            if (!sc.enabled) out << " [disabled]";
            if (!sc.settings.empty()) {
                out << " {";
                for (size_t i = 0; i < sc.settings.size(); ++i) {
                    if (i > 0) out << ", ";
                    out << sc.settings[i].key << "=" << sc.settings[i].value;
                }
                out << "}";
            }
            out << "\n";
        }
    }
    return out.str();
}

std::string Engine::getSceneHierarchyFromScript(int maxObjects) const {
    auto typeName = [](ObjectType t) -> const char* {
        switch (t) {
            case ObjectType::Cube: return "Cube";
            case ObjectType::Sphere: return "Sphere";
            case ObjectType::Capsule: return "Capsule";
            case ObjectType::OBJMesh: return "OBJMesh";
            case ObjectType::Model: return "Model";
            case ObjectType::DirectionalLight: return "DirectionalLight";
            case ObjectType::PointLight: return "PointLight";
            case ObjectType::SpotLight: return "SpotLight";
            case ObjectType::AreaLight: return "AreaLight";
            case ObjectType::Camera: return "Camera";
            case ObjectType::PostFXNode: return "PostFXNode";
            case ObjectType::Mirror: return "Mirror";
            case ObjectType::Plane: return "Plane";
            case ObjectType::Torus: return "Torus";
            case ObjectType::Sprite: return "Sprite";
            case ObjectType::Sprite2D: return "Sprite2D";
            case ObjectType::Canvas: return "Canvas";
            case ObjectType::UIImage: return "UIImage";
            case ObjectType::UISlider: return "UISlider";
            case ObjectType::UIButton: return "UIButton";
            case ObjectType::UIText: return "UIText";
            case ObjectType::Empty: return "Empty";
            case ObjectType::Sprite25D: return "Sprite25D";
            case ObjectType::Light2D: return "Light2D";
            case ObjectType::ShadowCaster2D: return "ShadowCaster2D";
            case ObjectType::ParticleSystem2D: return "ParticleSystem2D";
            case ObjectType::ReflectionCast: return "ReflectionCast";
        }
        return "Object";
    };

    auto componentSummary = [](const SceneObject& o) -> std::string {
        std::vector<std::string> comps;
        if (o.hasRigidbody) comps.push_back("Rigidbody");
        if (o.hasRigidbody2D) comps.push_back("Rigidbody2D");
        if (o.hasCollider) comps.push_back("Collider");
        if (o.hasCollider2D) comps.push_back("Collider2D");
        if (o.hasLight) comps.push_back("Light");
        if (o.hasLight2D) comps.push_back("Light2D");
        if (o.hasCamera) comps.push_back("Camera");
        if (o.hasAudioSource) comps.push_back("Audio");
        if (o.hasShadowCaster2D) comps.push_back("ShadowCaster2D");
        if (o.hasParallaxLayer2D) comps.push_back("Parallax2D");
        if (o.hasCameraFollow2D) comps.push_back("CamFollow2D");
        if (o.hasPlayerController) comps.push_back("PlayerController");
        if (o.hasUI) comps.push_back("UI");
        std::string s;
        for (size_t i = 0; i < comps.size(); ++i) { if (i) s += ", "; s += comps[i]; }
        return s;
    };

    std::ostringstream out;
    const std::string sceneName = getCurrentSceneNameFromScript();
    out << "Scene";
    if (!sceneName.empty()) out << " \"" << sceneName << "\"";
    out << " (" << sceneObjects.size() << " objects)";
    if (sceneObjects.empty()) {
        out << "\n(scene is empty)\n";
        return out.str();
    }
    out << "\n";

    std::unordered_map<int, size_t> indexById;
    indexById.reserve(sceneObjects.size());
    for (size_t i = 0; i < sceneObjects.size(); ++i) {
        indexById[sceneObjects[i].id] = i;
    }

    int emitted = 0;
    bool truncated = false;
    std::unordered_set<int> visited;

    std::function<void(int, int)> emit = [&](int id, int depth) {
        if (truncated) return;
        const auto it = indexById.find(id);
        if (it == indexById.end()) return;
        if (!visited.insert(id).second) return;
        if (maxObjects > 0 && emitted >= maxObjects) { truncated = true; return; }
        const SceneObject& o = sceneObjects[it->second];
        ++emitted;
        out << std::string(static_cast<size_t>(depth) * 2, ' ')
            << "- " << o.name << " (id " << o.id << ") [" << typeName(o.type) << "]";
        if (!o.enabled) out << " (disabled)";
        if (o.id == selectedObjectId) out << " *selected*";
        const std::string comps = componentSummary(o);
        if (!comps.empty()) out << " {" << comps << "}";
        if (!o.meshPath.empty()) out << " mesh=" << fs::path(o.meshPath).filename().generic_string();
        if (!o.scripts.empty()) {
            out << " scripts=";
            for (size_t i = 0; i < o.scripts.size(); ++i) {
                if (i) out << ",";
                const ScriptComponent& sc = o.scripts[i];
                out << (sc.path.empty() ? sc.managedType
                                        : fs::path(sc.path).filename().generic_string());
            }
        }
        out << "\n";
        for (int childId : o.childIds) emit(childId, depth + 1);
    };

    for (const SceneObject& o : sceneObjects) {
        const bool isRoot = (o.parentId < 0) || (indexById.find(o.parentId) == indexById.end());
        if (isRoot) emit(o.id, 0);
    }
    for (const SceneObject& o : sceneObjects) {
        if (truncated) break;
        if (visited.find(o.id) == visited.end()) emit(o.id, 0);
    }

    if (truncated) out << "...(truncated at " << maxObjects << " objects)\n";
    return out.str();
}

namespace {
    std::string normalizeTypeToken(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            if (c == ' ' || c == '_' || c == '-' || c == '.') continue;
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    bool parseObjectTypeName(const std::string& raw, ObjectType& out) {
        const std::string k = normalizeTypeToken(raw);
        if (k == "empty") { out = ObjectType::Empty; return true; }
        if (k == "cube" || k == "box") { out = ObjectType::Cube; return true; }
        if (k == "sphere" || k == "ball") { out = ObjectType::Sphere; return true; }
        if (k == "capsule") { out = ObjectType::Capsule; return true; }
        if (k == "plane") { out = ObjectType::Plane; return true; }
        if (k == "torus" || k == "donut") { out = ObjectType::Torus; return true; }
        if (k == "mirror") { out = ObjectType::Mirror; return true; }
        if (k == "objmesh") { out = ObjectType::OBJMesh; return true; }
        if (k == "model") { out = ObjectType::Model; return true; }
        if (k == "sprite") { out = ObjectType::Sprite; return true; }
        if (k == "sprite2d") { out = ObjectType::Sprite2D; return true; }
        if (k == "sprite25d") { out = ObjectType::Sprite25D; return true; }
        if (k == "camera") { out = ObjectType::Camera; return true; }
        if (k == "pointlight" || k == "point") { out = ObjectType::PointLight; return true; }
        if (k == "spotlight" || k == "spot") { out = ObjectType::SpotLight; return true; }
        if (k == "directionallight" || k == "directional" || k == "sun") { out = ObjectType::DirectionalLight; return true; }
        if (k == "arealight" || k == "area") { out = ObjectType::AreaLight; return true; }
        if (k == "light2d" || k == "2dlight") { out = ObjectType::Light2D; return true; }
        if (k == "shadowcaster2d") { out = ObjectType::ShadowCaster2D; return true; }
        if (k == "particlesystem2d" || k == "particles" || k == "particlesystem") { out = ObjectType::ParticleSystem2D; return true; }
        if (k == "canvas") { out = ObjectType::Canvas; return true; }
        if (k == "uiimage" || k == "image") { out = ObjectType::UIImage; return true; }
        if (k == "uitext" || k == "text" || k == "label") { out = ObjectType::UIText; return true; }
        if (k == "uibutton" || k == "button") { out = ObjectType::UIButton; return true; }
        if (k == "uislider" || k == "slider") { out = ObjectType::UISlider; return true; }
        return false;
    }
} // namespace

int Engine::createSceneObjectFromScript(const std::string& type, const std::string& name, int parentId) {
    ScriptSceneMutationScope mutation(this);
    ObjectType objType = ObjectType::Empty;
    if (!parseObjectTypeName(type, objType)) {
        return -1;
    }
    recordState("ai-create-object");
    const int id = nextObjectId++;
    std::string objName = name;
    while (!objName.empty() && (objName.front() == ' ' || objName.front() == '\t')) objName.erase(objName.begin());
    while (!objName.empty() && (objName.back() == ' ' || objName.back() == '\t')) objName.pop_back();
    if (objName.empty()) {
        objName = type + " " + std::to_string(id);
    }
    SceneObject obj(objName, ObjectType::Empty, id);
    ApplyObjectPreset(obj, objType);
    obj.localPosition = obj.position;
    obj.localRotation = NormalizeEulerDegrees(obj.rotation);
    obj.localScale = obj.scale;
    obj.localInitialized = true;
    sceneObjects.push_back(obj);
    markRuntimeScriptBindingsDirty();

    if (parentId >= 0) {
        SceneObject* parent = findObjectById(parentId);
        SceneObject* child = findObjectById(id);
        if (parent && child) {
            child->parentId = parentId;
            parent->childIds.push_back(id);
        }
    }
    updateHierarchyWorldTransforms();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    logToConsole("AI created: " + objName);
    return id;
}

bool Engine::deleteSceneObjectFromScript(int objectId) {
    ScriptSceneMutationScope mutation(this);
    if (!findObjectById(objectId)) {
        return false;
    }
    recordState("ai-delete-object");

    std::unordered_map<int, SceneObject*> idLookup;
    idLookup.reserve(sceneObjects.size());
    for (auto& obj : sceneObjects) idLookup.emplace(obj.id, &obj);

    std::unordered_set<int> toDelete;
    std::vector<int> stack;
    toDelete.insert(objectId);
    stack.push_back(objectId);
    while (!stack.empty()) {
        const int currentId = stack.back();
        stack.pop_back();
        auto it = idLookup.find(currentId);
        if (it == idLookup.end() || !it->second) continue;
        for (int childId : it->second->childIds) {
            if (childId >= 0 && toDelete.insert(childId).second) stack.push_back(childId);
        }
        for (const auto& obj : sceneObjects) {
            if (obj.parentId == currentId && toDelete.insert(obj.id).second) stack.push_back(obj.id);
        }
    }

    auto removeIt = std::remove_if(sceneObjects.begin(), sceneObjects.end(),
        [&toDelete](const SceneObject& obj) { return toDelete.count(obj.id) > 0; });
    if (removeIt == sceneObjects.end()) {
        return false;
    }
    sceneObjects.erase(removeIt, sceneObjects.end());
    markRuntimeScriptBindingsDirty();
    for (auto& obj : sceneObjects) {
        if (toDelete.count(obj.parentId) > 0) obj.parentId = -1;
        obj.childIds.erase(std::remove_if(obj.childIds.begin(), obj.childIds.end(),
            [&toDelete](int id) { return toDelete.count(id) > 0; }), obj.childIds.end());
    }
    if (toDelete.count(selectedObjectId) > 0) {
        clearSelection();
    }
    updateHierarchyWorldTransforms();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    logToConsole("AI deleted " + std::to_string(toDelete.size()) + " object(s)");
    return true;
}

bool Engine::renameSceneObjectFromScript(int objectId, const std::string& name) {
    SceneObject* obj = findObjectById(objectId);
    if (!obj || name.empty()) {
        return false;
    }
    recordState("ai-rename-object");
    obj->name = name;
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return true;
}

bool Engine::setSceneObjectParentFromScript(int objectId, int parentId) {
    ScriptSceneMutationScope mutation(this);
    if (!findObjectById(objectId)) {
        return false;
    }
    if (parentId >= 0 && !findObjectById(parentId)) {
        return false;
    }
    setParent(objectId, parentId, -1);
    updateHierarchyWorldTransforms();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return true;
}

bool Engine::setSceneObjectTransformFromScript(int objectId, const glm::vec3& position,
                                               const glm::vec3& rotationDeg, const glm::vec3& scale) {
    SceneObject* obj = findObjectById(objectId);
    if (!obj || obj->IsInvariable) {
        return false;
    }
    recordState("ai-set-transform");
    obj->position = position;
    obj->rotation = NormalizeEulerDegrees(rotationDeg);
    obj->scale = scale;
    syncLocalTransform(*obj);
    if (obj->hasRigidbody) {
        teleportPhysicsActorFromScript(obj->id, obj->position, obj->rotation);
    }
    updateHierarchyWorldTransforms();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return true;
}

bool Engine::setSceneObjectEnabledFromScript(int objectId, bool enabled) {
    SceneObject* obj = findObjectById(objectId);
    if (!obj) {
        return false;
    }
    if (obj->enabled != enabled) {
        recordState("ai-set-enabled");
        obj->enabled = enabled;
        markRuntimeScriptBindingsDirty();
        if (projectManager.currentProject.isLoaded) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
    }
    return true;
}

bool Engine::addObjectComponentFromScript(int objectId, const std::string& component) {
    SceneObject* obj = findObjectById(objectId);
    if (!obj) {
        return false;
    }
    const std::string k = normalizeTypeToken(component);
    bool handled = true;
    if (k == "rigidbody" || k == "rigidbody3d") obj->hasRigidbody = true;
    else if (k == "rigidbody2d") obj->hasRigidbody2D = true;
    else if (k == "collider" || k == "collider3d" || k == "boxcollider") obj->hasCollider = true;
    else if (k == "collider2d") obj->hasCollider2D = true;
    else if (k == "light") obj->hasLight = true;
    else if (k == "light2d") obj->hasLight2D = true;
    else if (k == "camera") obj->hasCamera = true;
    else if (k == "audio" || k == "audiosource") obj->hasAudioSource = true;
    else if (k == "shadowcaster2d") obj->hasShadowCaster2D = true;
    else if (k == "renderer") obj->hasRenderer = true;
    else if (k == "ui") obj->hasUI = true;
    else handled = false;

    if (!handled) {
        return false;
    }
    recordState("ai-add-component");
    markRuntimeScriptBindingsDirty();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return true;
}

bool Engine::attachObjectScriptFromScript(int objectId, const std::string& scriptPath) {
    ScriptSceneMutationScope mutation(this);
    SceneObject* obj = findObjectById(objectId);
    if (!obj) {
        return false;
    }
    const fs::path resolved = resolveProjectPathFromScript(scriptPath);
    std::error_code ec;
    if (resolved.empty() || !fs::exists(resolved, ec)) {
        return false;
    }
    ScriptComponent sc;
    std::string ext = resolved.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".cs") sc.language = ScriptLanguage::CSharp;
    else if (ext == ".c") sc.language = ScriptLanguage::C;
    else sc.language = ScriptLanguage::Cpp;
    sc.path = resolved.string();
    sc.lastBinaryPath.clear();
    sc.lastBinaryVerified = false;
    recordState("ai-attach-script");
    obj->scripts.push_back(std::move(sc));
    markRuntimeScriptBindingsDirty();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    logToConsole("AI attached script: " + resolved.filename().string());
    return true;
}

bool Engine::saveProjectFromScript() {
    if (!projectManager.currentProject.isLoaded) {
        return false;
    }
    return saveCurrentScene(false);
}

bool Engine::setRigidbodyVelocityFromScript(int id, const glm::vec3& velocity) {
    return physics->setLinearVelocity(id, velocity);
}

bool Engine::getRigidbodyVelocityFromScript(int id, glm::vec3& outVelocity) {
    return physics->getLinearVelocity(id, outVelocity);
}

bool Engine::getRigidbodyPoseFromScript(int id, glm::vec3& outPosition,
                                        glm::vec3& outRotationDeg) {
    return physics && physics->getActorPose(id, outPosition, outRotationDeg);
}

bool Engine::setContinuousCollisionDetectionFromScript(int id, bool enabled) {
    return physics && physics->setContinuousCollisionDetection(id, enabled);
}

int Engine::getObjectContactCountFromScript(int id) const {
    if (!physics) return 0;
    return physics->countActiveContacts(id);
}

bool Engine::getContactNormalFromScript(int id, glm::vec3& outNormal) const {
    if (!physics) return false;
    return physics->getContactNormal(id, outNormal);
}

bool Engine::areObjectsTouchingFromScript(int a, int b) const {
    if (!physics) return false;
    return physics->areObjectsTouching(a, b);
}

bool Engine::setObjectsCollisionIgnoredFromScript(int a, int b, bool ignored) {
    if (!physics) return false;
    return physics->setObjectsCollisionIgnored(a, b, ignored);
}

bool Engine::setRigidbodyAngularVelocityFromScript(int id, const glm::vec3& velocity) {
    return physics->setAngularVelocity(id, velocity);
}

bool Engine::getRigidbodyAngularVelocityFromScript(int id, glm::vec3& outVelocity) {
    return physics->getAngularVelocity(id, outVelocity);
}

bool Engine::teleportPhysicsActorFromScript(int id, const glm::vec3& position, const glm::vec3& rotationDeg) {
    return physics->setActorPose(id, position, rotationDeg);
}

float Engine::getProjectGravityScaleFromScript() const {
    if (!projectManager.currentProject.isLoaded) return 1.0f;
    return projectManager.currentProject.physicsSettings.globalGravityScale;
}

float Engine::getPhysicsFixedTimestepFromScript() const {
    const float configured = projectManager.currentProject.isLoaded
        ? projectManager.currentProject.physicsSettings.fixedTimestep
        : (1.0f / 60.0f);
    return std::clamp(configured, 0.001f, 0.1f);
}

void Engine::setProjectGravityScaleFromScript(float scale) {
    if (!projectManager.currentProject.isLoaded) return;
    projectManager.currentProject.physicsSettings.globalGravityScale = std::max(0.0f, scale);
    projectManager.currentProject.saveProjectFile();
    projectManager.currentProject.hasUnsavedChanges = true;
    physics->setProjectSettings(projectManager.currentProject.physicsSettings);
}

bool Engine::addRigidbodyForceFromScript(int id, const glm::vec3& force) {
    return physics->addForce(id, force);
}

bool Engine::addRigidbodyImpulseFromScript(int id, const glm::vec3& impulse) {
    return physics->addImpulse(id, impulse);
}

bool Engine::addRigidbodyTorqueFromScript(int id, const glm::vec3& torque) {
    return physics->addTorque(id, torque);
}

bool Engine::addRigidbodyAngularImpulseFromScript(int id, const glm::vec3& impulse) {
    return physics->addAngularImpulse(id, impulse);
}

bool Engine::setRigidbodyYawFromScript(int id, float yawDegrees) {
    return physics->setActorYaw(id, yawDegrees);
}

int Engine::getSelectedObjectId() const {
    return selectedObjectId;
}

bool Engine::raycastClosestFromScript(const glm::vec3& origin, const glm::vec3& dir, float distance,
                                      int ignoreId, glm::vec3* hitPos, glm::vec3* hitNormal,
                                      float* hitDistance, int* hitActorId,
                                      glm::vec3* hitActorVelocity,
                                      float* hitStaticFriction,
                                      float* hitDynamicFriction) const {
    return physics->raycastClosest(origin, dir, distance, ignoreId, hitPos, hitNormal, hitDistance,
                                  hitActorId, hitActorVelocity, hitStaticFriction, hitDynamicFriction);
}

void Engine::syncLocalTransform(SceneObject& obj) {
    if (obj.parentId == -1) {
        obj.localPosition = obj.position;
        obj.localRotation = NormalizeEulerDegrees(obj.rotation);
        obj.localScale = obj.scale;
        obj.localInitialized = true;
        return;
    }
    glm::vec3 parentPos(0.0f);
    glm::quat parentRot(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 parentScale(1.0f);
    if (obj.parentId != -1) {
        if (SceneObject* parent = findObjectById(obj.parentId)) {
            parentPos = parent->position;
            parentRot = QuatFromEulerXYZ(parent->rotation);
            parentScale = parent->scale;
        }
    }
    updateLocalFromWorld(obj, parentPos, parentRot, parentScale);
}

void Engine::setFrameRateCapFromScript(bool enabled, float cap) {
    fpsCapEnabled = enabled;
    fpsCap = std::max(1.0f, cap);
}

bool Engine::playAudioFromScript(int id) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAudioSource) return false;
    return audio.playObjectSound(*obj);
}

bool Engine::stopAudioFromScript(int id) {
    return audio.stopObjectSound(id);
}

bool Engine::setAudioLoopFromScript(int id, bool loop) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAudioSource) return false;
    obj->audioSource.loop = loop;
    markProjectDirty();
    return audio.setObjectLoop(*obj, loop);
}

bool Engine::setAudioVolumeFromScript(int id, float volume) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAudioSource) return false;
    obj->audioSource.volume = std::clamp(volume, 0.0f, 2.0f);
    markProjectDirty();
    return audio.setObjectVolume(*obj, obj->audioSource.volume);
}

bool Engine::setAudioClipFromScript(int id, const std::string& path) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAudioSource) return false;
    obj->audioSource.clipPath = path;
    markProjectDirty();
    // Ensure clip is loaded; do not auto-play unless PlayAudio is called.
    audio.setObjectLoop(*obj, obj->audioSource.loop);
    return true;
}

bool Engine::playAudioOneShotFromScript(int id, const std::string& clipPath, float volumeScale) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAudioSource) return false;
    return audio.playObjectOneShot(*obj, clipPath, volumeScale);
}

bool Engine::hasAnimationFromScript(int id) const {
    auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(), [id](const SceneObject& obj) {
        return obj.id == id;
    });
    if (it == sceneObjects.end()) return false;
    return it->hasAnimation && it->animation.enabled && !AnimationGetActiveClipAssetPath(it->animation).empty();
}

bool Engine::playAnimationFromScript(int id, bool restart) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAnimation || !obj->animation.enabled) return false;
    NormalizeAnimationClipSlots(obj->animation);
    const std::string activeClipPath = AnimationGetActiveClipAssetPath(obj->animation);
    if (activeClipPath.empty()) return false;

    obj->animation.runtimeInitialized = true;
    obj->animation.runtimeClipPath = activeClipPath;
    obj->animation.runtimeDirection = 1.0f;
    obj->animation.runtimePaused = false;
    obj->animation.runtimePlaying = true;

    const RuntimeAnimationClip* clip = getRuntimeAnimationClip(activeClipPath);
    const float duration = clip ? std::max(0.01f, clip->duration) : std::max(0.01f, obj->animation.clipLength);
    if (clip) {
        obj->animation.clipLength = duration;
    }
    if (restart) {
        obj->animation.runtimeTime = 0.0f;
    }
    obj->animation.runtimeTime = std::clamp(obj->animation.runtimeTime, 0.0f, duration);
    return true;
}

bool Engine::stopAnimationFromScript(int id, bool resetTime) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAnimation) return false;
    NormalizeAnimationClipSlots(obj->animation);
    const std::string activeClipPath = AnimationGetActiveClipAssetPath(obj->animation);
    if (activeClipPath.empty()) return false;

    obj->animation.runtimePlaying = false;
    obj->animation.runtimePaused = false;
    obj->animation.runtimeInitialized = true;
    obj->animation.runtimeClipPath = activeClipPath;
    if (resetTime) {
        obj->animation.runtimeTime = 0.0f;
    }
    return true;
}

bool Engine::pauseAnimationFromScript(int id, bool pause) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAnimation) return false;
    if (AnimationGetActiveClipAssetPath(obj->animation).empty()) return false;
    if (!obj->animation.runtimePlaying) return false;
    obj->animation.runtimePaused = pause;
    return true;
}

bool Engine::reverseAnimationFromScript(int id, bool restartIfStopped) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAnimation || !obj->animation.enabled) return false;
    NormalizeAnimationClipSlots(obj->animation);
    const std::string activeClipPath = AnimationGetActiveClipAssetPath(obj->animation);
    if (activeClipPath.empty()) return false;

    const RuntimeAnimationClip* clip = getRuntimeAnimationClip(activeClipPath);
    const float duration = clip ? std::max(0.01f, clip->duration) : std::max(0.01f, obj->animation.clipLength);
    if (clip) {
        obj->animation.clipLength = duration;
    }

    obj->animation.runtimeInitialized = true;
    obj->animation.runtimeClipPath = activeClipPath;
    obj->animation.runtimeDirection = -1.0f;
    if (!obj->animation.runtimePlaying && restartIfStopped) {
        obj->animation.runtimeTime = duration;
    }
    obj->animation.runtimeTime = std::clamp(obj->animation.runtimeTime, 0.0f, duration);
    obj->animation.runtimePaused = false;
    obj->animation.runtimePlaying = true;
    return true;
}

bool Engine::setAnimationTimeFromScript(int id, float timeSeconds) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAnimation) return false;
    NormalizeAnimationClipSlots(obj->animation);
    const std::string activeClipPath = AnimationGetActiveClipAssetPath(obj->animation);
    if (activeClipPath.empty()) return false;

    const RuntimeAnimationClip* clip = getRuntimeAnimationClip(activeClipPath);
    const float duration = clip ? std::max(0.01f, clip->duration) : std::max(0.01f, obj->animation.clipLength);
    if (clip) {
        obj->animation.clipLength = duration;
    }

    obj->animation.runtimeInitialized = true;
    obj->animation.runtimeClipPath = activeClipPath;
    obj->animation.runtimeTime = std::clamp(timeSeconds, 0.0f, duration);
    return true;
}

float Engine::getAnimationTimeFromScript(int id) const {
    auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(), [id](const SceneObject& obj) {
        return obj.id == id;
    });
    if (it == sceneObjects.end() || !it->hasAnimation) return 0.0f;
    return it->animation.runtimeTime;
}

bool Engine::isAnimationPlayingFromScript(int id) const {
    auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(), [id](const SceneObject& obj) {
        return obj.id == id;
    });
    if (it == sceneObjects.end() || !it->hasAnimation) return false;
    return it->animation.runtimePlaying && !it->animation.runtimePaused;
}

bool Engine::setAnimationLoopFromScript(int id, bool loop) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAnimation) return false;
    obj->animation.loop = loop;
    markProjectDirty();
    return true;
}

bool Engine::setAnimationPlaySpeedFromScript(int id, float speed) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAnimation) return false;
    obj->animation.playSpeed = std::max(0.0f, speed);
    markProjectDirty();
    return true;
}

bool Engine::setAnimationPlayOnAwakeFromScript(int id, bool playOnAwake) {
    SceneObject* obj = findObjectById(id);
    if (!obj || !obj->hasAnimation) return false;
    obj->animation.playOnAwake = playOnAwake;
    markProjectDirty();
    return true;
}
#pragma endregion

#pragma region Script Compilation + Editor Tabs
void Engine::ensureScriptHistoryLoaded() {
    if (!projectManager.currentProject.isLoaded) return;
    if (!scriptAutoCompileConfigValid) return;

    const fs::path historyPath = ScriptHistory::fileForOutDir(scriptAutoCompileConfig.outDir);
    if (historyPath.empty() || historyPath == scriptHistoryPath) return;

    scriptHistoryPath = historyPath;
    scriptHistory.load(scriptHistoryPath);
    scriptHistoryForcedRecompile.clear();
    if (scriptToolchainStamp.valid()) {
        scriptHistory.setStamp(scriptToolchainStamp);
    }
}

void Engine::recordScriptHistoryEntry(const fs::path& sourcePath, const fs::path& binaryPath) {
    if (sourcePath.empty() || binaryPath.empty()) return;
    if (!scriptToolchainStamp.valid()) return;

    fs::path resolvedSource = sourcePath;
    if (resolvedSource.is_relative() && projectManager.currentProject.isLoaded) {
        resolvedSource = projectManager.currentProject.projectPath / resolvedSource;
    }

    std::error_code ec;
    if (!fs::exists(binaryPath, ec) || ec) return;
    const fs::file_time_type binaryTime = fs::last_write_time(binaryPath, ec);
    if (ec) return;

    ScriptHistoryEntry entry;
    entry.stamp = scriptToolchainStamp;
    entry.binaryPath = binaryPath;
    entry.binaryWriteTime = ScriptHistory::toTimeRep(binaryTime);

    const std::string key = ScriptHistory::makeKey(resolvedSource);
    scriptHistory.record(key, entry);
    scriptHistoryForcedRecompile.erase(key);
    scriptLoadFailureRecompileRequested.erase(key);
}

void Engine::flushScriptHistory() {
    if (scriptHistoryPath.empty() || !scriptHistory.isDirty()) return;

    std::string error;
    if (!scriptHistory.save(scriptHistoryPath, error)) {
        std::cerr << "[Script] " << error << "\n";
    }
}

void Engine::requestRecompileForScriptLoadFailure(const fs::path& sourcePath,
                                                  ScriptLoadFailure failure) {
    if (failure != ScriptLoadFailure::AbiMismatch &&
        failure != ScriptLoadFailure::LayoutMismatch) {
        return;
    }
    if (sourcePath.empty() || !projectManager.currentProject.isLoaded) return;
    if (IsNativeBinaryPath(sourcePath)) return;

    fs::path resolvedSource = sourcePath;
    if (resolvedSource.is_relative()) {
        resolvedSource = projectManager.currentProject.projectPath / resolvedSource;
    }
    std::error_code ec;
    if (!fs::exists(resolvedSource, ec) || ec) return;

    const std::string key = ScriptHistory::makeKey(resolvedSource);
    if (!scriptLoadFailureRecompileRequested.insert(key).second) return;

    scriptHistory.forget(key);
    addConsoleMessage("Script '" + resolvedSource.filename().string() +
                          "' was built against an older engine ABI; recompiling",
                      ConsoleMessageType::Info);

    if (isPlaying || specMode || testMode) {
        scriptLoadFailurePendingSources.push_back(std::move(resolvedSource));
        return;
    }

    nextCompileFromAuto = true;
    queueScriptCompileBatch({resolvedSource});
    nextCompileFromAuto = false;
}

void Engine::requestScriptDomainReload(bool clearCompiledScripts) {
    scriptDomainReloadPending = true;
    scriptDomainReloadClearsCache = scriptDomainReloadClearsCache || clearCompiledScripts;
}

void Engine::updateScriptReloadRequests() {
    if (!scriptLoadFailurePendingSources.empty() && !isPlaying && !specMode && !testMode &&
        !compileInProgress) {
        std::vector<fs::path> batch;
        batch.swap(scriptLoadFailurePendingSources);
        nextCompileFromAuto = true;
        queueScriptCompileBatch(batch);
        nextCompileFromAuto = false;
    }

    if (!scriptDomainReloadPending) return;
    if (compileInProgress) return;

    scriptDomainReloadPending = false;
    const bool clearCache = scriptDomainReloadClearsCache;
    scriptDomainReloadClearsCache = false;
    reloadScriptDomain(clearCache);
}

void Engine::reloadScriptDomain(bool clearCompiledScripts) {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("No project is loaded", ConsoleMessageType::Warning);
        return;
    }
    if (isPlaying || specMode || testMode) {
        addConsoleMessage("Stop play/spec/test mode before reloading scripts",
                          ConsoleMessageType::Warning);
        return;
    }

    fs::path outDir = scriptAutoCompileConfigValid ? scriptAutoCompileConfig.outDir : fs::path();
    if (outDir.empty()) {
        ScriptBuildConfig config;
        std::string error;
        const fs::path configPath = resolveScriptsConfigPath(projectManager.currentProject);
        if (scriptCompiler.loadConfig(configPath, config, error)) {
            outDir = config.outDir;
        } else {
            outDir = projectManager.currentProject.projectPath / "Library" / "CompiledScripts";
        }
    }

    int removedFiles = 0;
    if (clearCompiledScripts && !outDir.empty()) {
        std::error_code ec;
        if (fs::exists(outDir, ec) && !ec) {
            std::vector<fs::path> doomed;
            for (fs::directory_iterator it(outDir, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                doomed.push_back(it->path());
            }
            for (const fs::path& victim : doomed) {
                std::error_code removeEc;
                const uintmax_t removed = fs::remove_all(victim, removeEc);
                if (!removeEc) removedFiles += static_cast<int>(removed);
            }
        }
        ec.clear();
        fs::create_directories(outDir, ec);
    }

    resetScriptRuntimeStateForReload(true);

    ++scriptAutoCompileScanGeneration;
    scriptLastAutoCompileTime.clear();
    scriptAutoCompileCheckedSourceTime.clear();
    scriptAutoCompileBinaryCache.clear();
    scriptAutoCompileDiscoveredSources.clear();
    scriptBinaryResolveCache.clear();
    scriptBuildConfigCache.clear();
    inspectorStaleRecompileAttempted.clear();
    scriptLoadFailureRecompileRequested.clear();
    scriptHistoryForcedRecompile.clear();
    autoCompileQueue.clear();
    autoCompileQueued.clear();
    managedAutoCompileHasCompiled = false;
    managedAutoCompileCompiledSource = fs::file_time_type{};

    if (clearCompiledScripts) {
        scriptHistory.clearEntries();
        flushScriptHistory();
    }

    scriptAutoCompileLastCheck = 0.0;
    scriptAutoCompileLastDirectoryScan = 0.0;

#if !MODULARITY_RUNTIME_ONLY
    refreshScriptingFileList();
#endif
    scriptEditorWindowsDirty = true;
    refreshScriptEditorWindows();
    deferInspectorRefresh = true;

    if (clearCompiledScripts) {
        addConsoleMessage("Reloaded script domain and cleared " + outDir.string() +
                              " (" + std::to_string(removedFiles) + " files)",
                          ConsoleMessageType::Success);
    } else {
        addConsoleMessage("Reloaded script domain", ConsoleMessageType::Success);
    }

#if !MODULARITY_RUNTIME_ONLY
    if (!scriptAutoCompileEnabled) {
        std::vector<fs::path> batch;
        batch.reserve(scriptingFileList.size());
        for (const fs::path& scriptPath : scriptingFileList) {
            std::string ext = scriptPath.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".cs" || ext == ".csproj") continue;
            batch.push_back(scriptPath);
        }
        if (!batch.empty()) {
            queueScriptCompileBatch(batch);
        }
    }
#endif
}

void Engine::resetScriptRuntimeStateForReload(bool clearBinaryPaths) {
    if (!scriptEditorWindows.empty()) {
        ScriptContext editorCtx;
        editorCtx.engine = this;
        editorCtx.object = getSelectedObject();
        editorCtx.script = nullptr;

        for (const auto& entry : scriptEditorWindows) {
            if (entry.open && !entry.binaryPath.empty()) {
                scriptRuntime.callExitEditorWindow(entry.binaryPath, editorCtx);
            }
        }
    }

    scriptRuntime.unloadAll();
    managedRuntime.unloadAll();

    for (SceneObject& obj : sceneObjects) {
        for (ScriptComponent& sc : obj.scripts) {
            sc.activeIEnums.clear();
            if (clearBinaryPaths) {
                sc.lastBinaryPath.clear();
                sc.lastBinaryVerified = false;
            }
        }
    }

    nativeScriptMissingLogged.clear();
    nativeScriptLoadErrorLogged.clear();
    markRuntimeScriptBindingsDirty();
    scriptEditorWindowsDirty = true;
}

void Engine::compileScriptFile(const fs::path& scriptPath) {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("No project is loaded", ConsoleMessageType::Warning);
        return;
    }

    std::string ext = scriptPath.extension().string();
    if (ext == ".cs" || ext == ".csproj") {
        compileManagedScripts();
        return;
    }

    if (static_cast<int>(compileWorkers.size()) >= compileMaxParallelJobs()) {
        compileUiToast = false;
        compilePopupOpened = false;
        showCompilePopup = true;
        lastCompileStatus = "Compile already in progress";
        return;
    }

    compileCurrentPath = scriptPath;
    compileCurrentLabel = scriptPath.filename().empty() ? scriptPath.string() : scriptPath.filename().string();
    compileCurrentManaged = false;
    if (compileBatchTotal <= 1 && compileRequestQueue.empty()) {
        compileHistory.clear();
        compileBatchTotal = 1;
        compileBatchCompleted = 0;
        compileCompletionStart = 0.0;
        compileUiToast = nextCompileFromAuto;
        playCompileStartSound();
    }

    showCompilePopup = true;
    compilePopupHideTime = 0.0;
    lastCompileLog.clear();
    lastCompileDiagnostics.clear();
    lastCompileStatus = "Compiling " + scriptPath.filename().string();
    lastCompileSuccess = false;
    {
        std::lock_guard<std::mutex> lock(compileMutex);
        compileProgress = 0.05f;
        compileStage = "Preparing";
    }

    fs::path configPath = resolveScriptsConfigPath(projectManager.currentProject);
    fs::path projectRoot = projectManager.currentProject.projectPath;

    compileInProgress = true;
    compileWorkers.push_back(std::async(std::launch::async,
                                         [this, scriptPath, configPath, projectRoot]() -> ScriptCompileJobResult {
        auto setProgress = [this](float value, const char* stage) {
            std::lock_guard<std::mutex> lock(compileMutex);
            compileProgress = value;
            compileStage = stage;
        };
        ScriptCompileJobResult result;
        result.scriptPath = scriptPath;
        std::string compileWorkerStage = "Starting";
        try {
            std::string error;
            ScriptBuildConfig config;
            compileWorkerStage = "Loading scripts config";
            if (!scriptCompiler.loadConfig(configPath, config, error)) {
                result.error = error;
            } else {
                compileWorkerStage = "Applying package build config";
                packageManager.applyToBuildConfig(config);
                bool toolchainReady = true;
#ifdef __ANDROID__
                compileWorkerStage = "Preparing on-device compiler";
                {
                    std::string toolchainError;
                    toolchainReady = configureOnDeviceScriptCompile(config, toolchainError);
                    if (!toolchainReady) error = toolchainError;
                }
#endif
                ScriptBuildCommands commands;
                compileWorkerStage = "Preparing native script commands";
                bool commandsReady = false;
                if (toolchainReady) {
                    try {
                        commandsReady = scriptCompiler.makeCommands(config, scriptPath, commands, error);
                    } catch (const std::regex_error& e) {
                        error = std::string("Preparing native script commands failed in regex processing: ") + e.what();
                    }
                }
                if (!commandsReady) {
                    result.error = error;
                } else {
                    setProgress(0.15f, "Compiling");
                    compileWorkerStage = "Running native script compiler";
                    ScriptCompileOutput output;
                    if (!scriptCompiler.compile(commands, output, error)) {
                        result.compileLog = output.compileLog;
                        result.linkLog = output.linkLog;
                        result.error = error;
                        setProgress(0.9f, "Finalizing");
                    } else {
                        compileWorkerStage = "Resolving compiled script path";
                        result.success = true;
                        result.compileLog = output.compileLog;
                        result.linkLog = output.linkLog;
                        result.binaryPath = commands.binaryPath;
                        result.stagedBinaryPath = output.producedBinaryPath;

                        fs::path compiledSourcePath = scriptPath;
                        if (compiledSourcePath.is_relative()) {
                            std::error_code projectEc;
                            fs::path projectCandidate = projectRoot / compiledSourcePath;
                            if (fs::exists(projectCandidate, projectEc) && !projectEc) {
                                compiledSourcePath = projectCandidate;
                            }
                        }

                        std::error_code sourceEc;
                        fs::path sourceAbs = fs::absolute(compiledSourcePath, sourceEc);
                        if (sourceEc) sourceAbs = compiledSourcePath;
                        fs::path sourceCanonical = fs::weakly_canonical(sourceAbs, sourceEc);
                        if (!sourceEc) sourceAbs = sourceCanonical;
                        result.compiledSource = sourceAbs.lexically_normal().string();
                        setProgress(0.85f, "Reloading");
                    }
                }
            }
            compileWorkerStage = "Collecting script diagnostics";
            result.diagnostics = Modularity::collectScriptDiagnostics(
                result.scriptPath,
                result.error,
                result.compileLog,
                result.linkLog,
                result.isManaged);
        } catch (const std::exception& e) {
            result.success = false;
            result.error = "Script compile worker failed during " + compileWorkerStage + ": " + e.what();
            result.diagnostics = Modularity::collectScriptDiagnostics(
                result.scriptPath,
                result.error,
                result.compileLog,
                result.linkLog,
                result.isManaged);
        } catch (...) {
            result.success = false;
            result.error = "Script compile worker failed during " + compileWorkerStage +
                           " with an unknown exception.";
            result.diagnostics = Modularity::collectScriptDiagnostics(
                result.scriptPath,
                result.error,
                result.compileLog,
                result.linkLog,
                result.isManaged);
        }

        return result;
    }));
}

void Engine::compileManagedScripts() {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("No project is loaded", ConsoleMessageType::Warning);
        return;
    }

    if (!compileWorkers.empty()) {
        showCompilePopup = true;
        lastCompileStatus = "Compile already in progress";
        return;
    }

    fs::path projectRoot = projectManager.currentProject.projectPath;
    fs::path projectManagedProject = projectRoot / "Scripts" / "Managed" / "ModuCPP.csproj";
    compileCurrentPath = projectManagedProject;
    compileCurrentLabel = "Managed Scripts";
    compileCurrentManaged = true;
    if (compileBatchTotal <= 1 && compileRequestQueue.empty()) {
        compileHistory.clear();
        compileBatchTotal = 1;
        compileBatchCompleted = 0;
        compileCompletionStart = 0.0;
        compileUiToast = nextCompileFromAuto;
        playCompileStartSound();
    }
    if (!fs::exists(projectManagedProject)) {
        std::string setupError;
        fs::path engineRoot = findEngineManagedRoot();
        if (!engineRoot.empty()) {
            ensureProjectManagedCsproj(projectRoot, engineRoot, setupError);
        } else {
            setupError = "Managed project setup failed: engine root not found.";
        }
        if (!setupError.empty()) {
            lastCompileLog = setupError;
            lastCompileDiagnostics = Modularity::collectScriptDiagnostics(
                projectManagedProject,
                setupError,
                {},
                {},
                true);
            lastCompileStatus = "Compile failed";
            showCompilePopup = true;
            compilePopupHideTime = glfwGetTime() + 1.5;
            if (!lastCompileDiagnostics.empty()) {
                addConsoleMessage(
                    Modularity::formatScriptDiagnostic(lastCompileDiagnostics.front()),
                    DiagnosticConsoleType(lastCompileDiagnostics.front().severity));
            } else {
                addConsoleMessage(setupError, ConsoleMessageType::Error);
            }
        }
    }

    fs::path managedProject = fs::exists(projectManagedProject)
        ? projectManagedProject
        : getManagedProjectPath();
    if (!fs::exists(managedProject)) {
        const std::string managedProjectError = "Managed project not found: " + managedProject.string();
        lastCompileLog = managedProjectError;
        lastCompileDiagnostics = Modularity::collectScriptDiagnostics(
            managedProject,
            managedProjectError,
            {},
            {},
            true);
        lastCompileStatus = "Compile failed";
        showCompilePopup = true;
        compilePopupHideTime = glfwGetTime() + 1.5;
        if (!lastCompileDiagnostics.empty()) {
            addConsoleMessage(
                Modularity::formatScriptDiagnostic(lastCompileDiagnostics.front()),
                DiagnosticConsoleType(lastCompileDiagnostics.front().severity));
        } else {
            addConsoleMessage("Managed project not found: " + managedProject.string(), ConsoleMessageType::Error);
        }
        return;
    }

    showCompilePopup = true;
    compilePopupHideTime = 0.0;
    lastCompileLog.clear();
    lastCompileDiagnostics.clear();
    lastCompileStatus = "Compiling managed scripts";
    lastCompileSuccess = false;
    {
        std::lock_guard<std::mutex> lock(compileMutex);
        compileProgress = 0.05f;
        compileStage = "Preparing";
    }

    compileInProgress = true;
    compileWorkers.push_back(std::async(std::launch::async, [this, managedProject]() -> ScriptCompileJobResult {
        auto setProgress = [this](float value, const char* stage) {
            std::lock_guard<std::mutex> lock(compileMutex);
            compileProgress = value;
            compileStage = stage;
        };

        ScriptCompileJobResult result;
        result.isManaged = true;
        result.scriptPath = managedProject;

        try {
            setProgress(0.2f, "Building");

            std::string command = "dotnet build \"" + managedProject.string() + "\" -c Debug 2>&1";
            std::string output;
            int exitCode = -1;

            auto runCommand = [&]() -> bool {
#if defined(_WIN32)
                FILE* pipe = _popen(command.c_str(), "r");
#else
                FILE* pipe = popen(command.c_str(), "r");
#endif
                if (!pipe) return false;
                char buffer[256];
                while (fgets(buffer, sizeof(buffer), pipe)) {
                    output += buffer;
                }
#if defined(_WIN32)
                exitCode = _pclose(pipe);
#else
                exitCode = pclose(pipe);
#endif
                return true;
            };

            if (!runCommand()) {
                result.error = "Failed to launch dotnet build";
                result.compileLog = output;
            } else if (exitCode != 0) {
                result.error = "dotnet build failed";
                result.compileLog = output;
            } else {
                result.success = true;
                result.compileLog = output;
                result.binaryPath = managedOutputPathFromProject(managedProject);
                result.compiledSource = managedProject.string();
                setProgress(0.85f, "Reloading");
            }

            result.diagnostics = Modularity::collectScriptDiagnostics(
                result.scriptPath,
                result.error,
                result.compileLog,
                result.linkLog,
                result.isManaged);
        } catch (const std::exception& e) {
            result.success = false;
            result.error = std::string("Managed compile worker failed: ") + e.what();
            result.diagnostics = Modularity::collectScriptDiagnostics(
                result.scriptPath,
                result.error,
                result.compileLog,
                result.linkLog,
                result.isManaged);
        } catch (...) {
            result.success = false;
            result.error = "Managed compile worker failed with an unknown exception.";
            result.diagnostics = Modularity::collectScriptDiagnostics(
                result.scriptPath,
                result.error,
                result.compileLog,
                result.linkLog,
                result.isManaged);
        }

        return result;
    }));
}

void Engine::updateCompileJob() {
    ScriptCompileJobResult result;
    bool haveResult = false;
    for (size_t i = 0; i < compileWorkers.size(); ++i) {
        if (compileWorkers[i].wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            result = compileWorkers[i].get();
            compileWorkers.erase(compileWorkers.begin() + static_cast<long>(i));
            haveResult = true;
            break;
        }
    }

    if (compileBatchTotal > 1) {
        const float completedFrac =
            static_cast<float>(compileBatchCompleted) / static_cast<float>(compileBatchTotal);
        const float inFlightCredit =
            0.5f * static_cast<float>(compileWorkers.size()) / static_cast<float>(compileBatchTotal);
        std::lock_guard<std::mutex> lock(compileMutex);
        compileProgress = std::clamp(completedFrac + inFlightCredit, 0.0f, 0.98f);
        compileStage = "Compiling";
    }

    if (haveResult) {
        const auto __moduFinalizeStart = std::chrono::steady_clock::now();

        lastCompileDiagnostics = result.diagnostics;
        const int warningCount = static_cast<int>(std::count_if(
            lastCompileDiagnostics.begin(),
            lastCompileDiagnostics.end(),
            [](const Modularity::ScriptDiagnostic& diagnostic) {
                return diagnostic.severity == Modularity::ScriptDiagnosticSeverity::Warning;
            }));
        const bool finishedWithWarnings = warningCount > 0;
        const std::string displayLabel = result.scriptPath.filename().empty()
            ? (result.isManaged ? "Managed Scripts" : "Script")
            : result.scriptPath.filename().string();

        auto logDiagnostic = [&](const Modularity::ScriptDiagnostic& diagnostic) {
            addConsoleMessage(Modularity::formatScriptDiagnostic(diagnostic),
                              DiagnosticConsoleType(diagnostic.severity));
        };

        auto logRawCompilerOutputIfUnparsed = [&]() {
            const bool locatedAnyDiagnostic = std::any_of(
                lastCompileDiagnostics.begin(), lastCompileDiagnostics.end(),
                [](const Modularity::ScriptDiagnostic& d) { return d.line > 0; });
            if (locatedAnyDiagnostic) return;

            std::string rawOutput = result.compileLog;
            if (!result.linkLog.empty()) {
                if (!rawOutput.empty() && rawOutput.back() != '\n') rawOutput += '\n';
                rawOutput += result.linkLog;
            }
            while (!rawOutput.empty() &&
                   std::isspace(static_cast<unsigned char>(rawOutput.back()))) {
                rawOutput.pop_back();
            }
            if (rawOutput.empty()) return;

            addConsoleMessage("Compiler output (" + displayLabel + "):\n" + rawOutput,
                              ConsoleMessageType::Error);
        };

        if (!result.success) {
            lastCompileSuccess = false;
            lastCompileStatus = "Compile failed (" + displayLabel + ")";
            lastCompileLog = result.compileLog + result.linkLog + result.error;
            logRawCompilerOutputIfUnparsed();
            if (lastCompileDiagnostics.empty()) {
                if (!result.error.empty()) {
                    addConsoleMessage("Compile failed (" + displayLabel + "): " + result.error, ConsoleMessageType::Error);
                } else {
                    addConsoleMessage("Compile failed (" + displayLabel + ")", ConsoleMessageType::Error);
                }
            } else {
                for (const auto& diagnostic : lastCompileDiagnostics) {
                    logDiagnostic(diagnostic);
                }
            }
            playEditorSound(EditorSoundCue::Error, 0.95f);
        } else {
            if (!result.isManaged && !result.stagedBinaryPath.empty() &&
                result.stagedBinaryPath != result.binaryPath) {
                resetScriptRuntimeStateForReload(false);

                std::error_code stageEc;
                fs::create_directories(result.binaryPath.parent_path(), stageEc);
                stageEc.clear();
                if (fs::exists(result.binaryPath, stageEc) && !stageEc) {
                    fs::remove(result.binaryPath, stageEc);
                    stageEc.clear();
                }

                fs::rename(result.stagedBinaryPath, result.binaryPath, stageEc);
                if (stageEc) {
                    stageEc.clear();
                    fs::copy_file(result.stagedBinaryPath, result.binaryPath,
                                  fs::copy_options::overwrite_existing, stageEc);
                    if (!stageEc) {
                        std::error_code removeEc;
                        fs::remove(result.stagedBinaryPath, removeEc);
                    }
                }

                if (stageEc) {
                    result.success = false;
                    result.error = "Failed to finalize linked script binary: " + result.binaryPath.string();
                    result.diagnostics = Modularity::collectScriptDiagnostics(
                        result.scriptPath,
                        result.error,
                        result.compileLog,
                        result.linkLog,
                        result.isManaged);
                    lastCompileDiagnostics = result.diagnostics;
                }
            }

            if (!result.success) {
                lastCompileSuccess = false;
                lastCompileStatus = "Compile failed (" + displayLabel + ")";
                lastCompileLog = result.compileLog + result.linkLog + result.error;
                logRawCompilerOutputIfUnparsed();
                if (lastCompileDiagnostics.empty()) {
                    if (!result.error.empty()) {
                        addConsoleMessage("Compile failed (" + displayLabel + "): " + result.error, ConsoleMessageType::Error);
                    } else {
                        addConsoleMessage("Compile failed (" + displayLabel + ")", ConsoleMessageType::Error);
                    }
                } else {
                    for (const auto& diagnostic : lastCompileDiagnostics) {
                        logDiagnostic(diagnostic);
                    }
                }
                playEditorSound(EditorSoundCue::Error, 0.95f);
            } else {
            lastCompileSuccess = true;
            lastCompileLog = result.compileLog + result.linkLog;
            lastCompileStatus = finishedWithWarnings
                ? "Finished compiling (" + displayLabel + ") with warnings"
                : "Finished compiling (" + displayLabel + ")";
            playEditorSound(EditorSoundCue::CompileSuccess, 0.95f);
            addConsoleMessage(
                finishedWithWarnings
                    ? "Finished compiling (" + displayLabel + ") with warnings"
                    : "Finished compiling (" + displayLabel + ")",
                finishedWithWarnings ? ConsoleMessageType::Warning : ConsoleMessageType::Success);
            for (const auto& diagnostic : lastCompileDiagnostics) {
                if (diagnostic.severity == Modularity::ScriptDiagnosticSeverity::Info) {
                    continue;
                }
                logDiagnostic(diagnostic);
            }

            if (result.isManaged) {
                for (auto& obj : sceneObjects) {
                    for (auto& sc : obj.scripts) {
                        if (sc.language != ScriptLanguage::CSharp) continue;
                        sc.lastBinaryPath = result.binaryPath.string();
                        sc.lastBinaryVerified = true;
                    }
                }
            } else {
                auto normalizeSourcePath = [&](const fs::path& path, bool treatAsProjectRelative) {
                    if (path.empty()) return std::string();

                    fs::path candidate = path;
                    if (treatAsProjectRelative && candidate.is_relative() &&
                        projectManager.currentProject.isLoaded) {
                        candidate = projectManager.currentProject.projectPath / candidate;
                    }

                    std::error_code ec;
                    fs::path absolutePath = fs::absolute(candidate, ec);
                    if (ec) absolutePath = candidate;
                    fs::path canonicalPath = fs::weakly_canonical(absolutePath, ec);
                    if (!ec) absolutePath = canonicalPath;

                    std::string normalized = absolutePath.lexically_normal().string();
#if defined(_WIN32)
                    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
                    return normalized;
                };

                const std::string compiledFromSource = normalizeSourcePath(
                    result.compiledSource.empty() ? result.scriptPath : fs::path(result.compiledSource), false);
                const std::string compiledFromRequest = normalizeSourcePath(result.scriptPath, true);
                const std::string compiledBinary = result.binaryPath.string();

                recordScriptHistoryEntry(result.scriptPath, result.binaryPath);

                for (auto& obj : sceneObjects) {
                    for (auto& sc : obj.scripts) {
                        if (sc.language == ScriptLanguage::CSharp) continue;

                        const std::string scriptSource = normalizeSourcePath(sc.path, true);
                        const bool isCompiledScript =
                            (!compiledFromSource.empty() && scriptSource == compiledFromSource) ||
                            (!compiledFromRequest.empty() && scriptSource == compiledFromRequest);

                        if (isCompiledScript) {
                            sc.lastBinaryPath = compiledBinary;
                            sc.lastBinaryVerified = !compiledBinary.empty();
                        }
                    }
                }
            }
            }
        }

        ScriptCompileHistoryItem history;
        history.scriptPath = result.scriptPath;
        history.displayLabel = result.scriptPath.filename().empty() ? result.scriptPath.string()
                                                                   : result.scriptPath.filename().string();
        if (history.displayLabel.empty()) {
            history.displayLabel = result.isManaged ? "Managed Scripts" : "Script";
        }
        history.statusLabel = result.success
            ? (finishedWithWarnings ? "Completed with warnings" : "Completed")
            : "Failed";
        history.summary = result.success
            ? (finishedWithWarnings ? "Completed with warnings" : "Completed")
            : (result.error.empty() ? "Compile failed" : result.error);
        history.outputLog = result.compileLog + result.linkLog + result.error;
        history.diagnostics = lastCompileDiagnostics;
        history.success = result.success;
        history.warning = finishedWithWarnings;
        history.completedAt = glfwGetTime();
        compileHistory.push_back(std::move(history));
        if (compileHistory.size() > 18) {
            compileHistory.erase(compileHistory.begin());
        }

        ++compileBatchCompleted;

        if (compileBatchCompleted >= compileBatchTotal) {
            std::lock_guard<std::mutex> lock(compileMutex);
            compileProgress = 1.0f;
            compileStage = lastCompileSuccess ? "Done" : "Failed";
        }

        startQueuedCompileJobs();

        if (!compileRequestQueue.empty() || !compileWorkers.empty()) {
            return;
        }

        {
            // Refresh runtime state once after the entire batch finishes.
            auto __t0 = std::chrono::steady_clock::now();
            resetScriptRuntimeStateForReload(false);
            auto __t1 = std::chrono::steady_clock::now();
            scriptEditorWindowsDirty = true;
            refreshScriptEditorWindows();
            auto __t2 = std::chrono::steady_clock::now();
            nativeScriptMissingLogged.clear();
            nativeScriptLoadErrorLogged.clear();
            if (result.success && !result.isManaged && !result.binaryPath.empty()) {
                (void)scriptRuntime.getInspector(result.binaryPath);
            }
            auto __t3 = std::chrono::steady_clock::now();
            compileCompletionStart = glfwGetTime();
            auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            std::fprintf(stderr,
                "[ModuTimer]   reset=%.2f refreshWindows=%.2f preWarmInspector=%.2f\n",
                ms(__t0, __t1), ms(__t1, __t2), ms(__t2, __t3));
        }
        compileBatchTotal = 0;
        compileBatchCompleted = 0;
        compileCurrentManaged = false;
        compileInProgress = false;
        showCompilePopup = true;

        // sat there forever lol. toasts now fade on their own; the modal closes on success but
        const double finishedAt = glfwGetTime();
        if (compileUiToast) {
            compilePopupHideTime = finishedAt + (lastCompileSuccess ? 1.6 : 4.0);
        } else if (lastCompileSuccess && !finishedWithWarnings) {
            compilePopupHideTime = finishedAt + 1.6;
        } else {
            compilePopupHideTime = 0.0;
        }

        const double __moduFinalizeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - __moduFinalizeStart).count();
        std::fprintf(stderr, "[ModuTimer] post-compile finalize %.2f ms  %s\n",
                     __moduFinalizeMs, result.scriptPath.string().c_str());
    }

    if (!compileInProgress && managedAutoCompileQueued) {
        managedAutoCompileQueued = false;
        nextCompileFromAuto = true;
        compileManagedScripts();
        nextCompileFromAuto = false;
    }
}

fs::path Engine::scriptEditorWindowProbeCachePath() const {
    if (!projectManager.currentProject.isLoaded) return {};
    return projectManager.currentProject.projectPath / "Library" / "EditorWindowProbe.cache";
}

void Engine::loadScriptEditorWindowProbeCache() {
    if (scriptEditorWindowProbeCacheLoaded) return;
    scriptEditorWindowProbeCacheLoaded = true;

    const fs::path cachePath = scriptEditorWindowProbeCachePath();
    if (cachePath.empty()) return;
    std::ifstream file(cachePath);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const size_t firstBar = line.find('|');
        if (firstBar == std::string::npos) continue;
        const size_t secondBar = line.find('|', firstBar + 1);
        if (secondBar == std::string::npos) continue;
        const std::string path = line.substr(secondBar + 1);
        if (path.empty()) continue;
        try {
            const long long ticks = std::stoll(line.substr(0, firstBar));
            ScriptEditorWindowProbe probe;
            probe.writeTime = fs::file_time_type(fs::file_time_type::duration(ticks));
            probe.hasEditorWindow = (line[firstBar + 1] == '1');
            scriptEditorWindowProbeCache[path] = probe;
        } catch (...) {
        }
    }
}

void Engine::saveScriptEditorWindowProbeCache() {
    if (!scriptEditorWindowProbeCacheDirty) return;
    const fs::path cachePath = scriptEditorWindowProbeCachePath();
    if (cachePath.empty()) return;

    std::error_code ec;
    fs::create_directories(cachePath.parent_path(), ec);
    std::ofstream file(cachePath, std::ios::trunc);
    if (!file.is_open()) return;
    for (const auto& entry : scriptEditorWindowProbeCache) {
        // The cache reader uses stoll; libc++ file clocks may use __int128,
        // which has no ostream insertion operator.
        file << static_cast<long long>(entry.second.writeTime.time_since_epoch().count()) << '|'
             << (entry.second.hasEditorWindow ? '1' : '0') << '|'
             << entry.first << '\n';
    }
    scriptEditorWindowProbeCacheDirty = false;
}

void Engine::refreshScriptEditorWindows() {
    if (!scriptEditorWindowsDirty) return;
    scriptEditorWindowsDirty = false;
    struct ProbeCacheFlush {
        Engine* engine;
        ~ProbeCacheFlush() { engine->saveScriptEditorWindowProbeCache(); }
    } __probeFlush{this};

    if (!projectManager.currentProject.isLoaded) {
        scriptEditorWindows.clear();
        return;
    }
    auto __rA = std::chrono::steady_clock::now();
    int __sceneCount = 0;
    int __dirCount = 0;

    std::unordered_map<std::string, bool> previousState;
    for (const auto& entry : scriptEditorWindows) {
        previousState[entry.binaryPath.lexically_normal().string()] = entry.open;
    }

    std::unordered_set<std::string> seen;
    std::vector<ScriptEditorWindowEntry> updated;

    loadScriptEditorWindowProbeCache();

    auto tryAddEntry = [&](const fs::path& binaryPath) {
        if (binaryPath.empty() || !fs::exists(binaryPath)) return;
        std::string key = binaryPath.lexically_normal().string();
        if (!seen.insert(key).second) return;

        std::error_code probeEc;
        const fs::file_time_type writeTime = fs::last_write_time(binaryPath, probeEc);
        if (!probeEc) {
            auto cacheIt = scriptEditorWindowProbeCache.find(key);
            if (cacheIt != scriptEditorWindowProbeCache.end() &&
                cacheIt->second.writeTime == writeTime) {
                if (!cacheIt->second.hasEditorWindow) return;
            } else {
                const bool probed = scriptRuntime.hasEditorWindow(binaryPath);
                ScriptEditorWindowProbe probe;
                probe.writeTime = writeTime;
                probe.hasEditorWindow = probed;
                scriptEditorWindowProbeCache[key] = probe;
                scriptEditorWindowProbeCacheDirty = true;
                if (!probed) return;
            }
        } else if (!scriptRuntime.hasEditorWindow(binaryPath)) {
            return;
        }

        ScriptEditorWindowEntry entry;
        entry.binaryPath = binaryPath;
        entry.label = binaryPath.stem().string();
        if (entry.label.empty()) entry.label = "ScriptWindow";
        auto it = previousState.find(key);
        entry.open = (it != previousState.end()) ? it->second : false;
        updated.push_back(std::move(entry));
    };

    for (const auto& obj : sceneObjects) {
        for (const auto& sc : obj.scripts) {
            ++__sceneCount;
            if (sc.lastBinaryPath.empty()) continue;
            fs::path binaryPath(sc.lastBinaryPath);
            std::error_code __ec;
            if (!fs::exists(binaryPath, __ec)) continue;
            tryAddEntry(binaryPath);
        }
    }
    auto __rB = std::chrono::steady_clock::now();

    // Also scan the configured script output directory for standalone editor tabs.
    fs::path configPath = resolveScriptsConfigPath(projectManager.currentProject);
    ScriptBuildConfig config;
    std::string error;
    if (scriptCompiler.loadConfig(configPath, config, error)) {
        fs::path outDir = config.outDir;
        if (!outDir.is_absolute()) {
            outDir = projectManager.currentProject.projectPath / outDir;
        }
        std::error_code ec;
        if (fs::exists(outDir, ec)) {
            for (auto it = fs::recursive_directory_iterator(outDir, ec);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (it->is_directory()) {
                    const std::string dirName = it->path().filename().string();
                    if (dirName == ".loaded" || dirName == ".staging") {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                ++__dirCount;
                auto ext = it->path().extension().string();
                if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                    tryAddEntry(it->path());
                }
            }
        }
    }
    auto __rC = std::chrono::steady_clock::now();

    scriptEditorWindows.swap(updated);
    auto msF = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::fprintf(stderr,
        "[ModuTimer]   refresh: sceneScripts=%.2f (%d) dirScan=%.2f (%d files)\n",
        msF(__rA, __rB), __sceneCount, msF(__rB, __rC), __dirCount);
}

void Engine::renderScriptEditorWindows() {
    if (scriptEditorWindows.empty()) return;

    ScriptContext ctx;
    ctx.engine = this;
    ctx.object = getSelectedObject();
    ctx.script = nullptr;

    ImGuiID scriptDockId = 0;
    const char* dockAnchors[] = { "Scripting", "Inspector", "Project", "Viewport" };
    for (const char* anchorName : dockAnchors) {
        ImGuiWindow* anchorWindow = ImGui::FindWindowByName(anchorName);
        if (!anchorWindow || anchorWindow->DockId == 0) continue;
        if (ImGui::DockBuilderGetNode(anchorWindow->DockId) == nullptr) continue;
        scriptDockId = anchorWindow->DockId;
        break;
    }

    for (auto& entry : scriptEditorWindows) {
        if (!entry.open) continue;

        if (scriptDockId != 0) {
            ImGui::SetNextWindowDockID(scriptDockId, ImGuiCond_FirstUseEver);
        }
        std::string title = entry.label + "###" + entry.binaryPath.string();
        if (ImGui::Begin(title.c_str(), &entry.open)) {
            scriptRuntime.callEditorWindow(entry.binaryPath, ctx);
        }
        ImGui::End();

        if (!entry.open) {
            scriptRuntime.callExitEditorWindow(entry.binaryPath, ctx);
        }
    }
}
#pragma endregion

#pragma region ImGui Setup
void Engine::setupImGui() {
    float mainScale = 1.0f;
#if defined(__ANDROID__)
    mainScale = Modularity::AndroidRuntime::GetDisplayDensityScale() * 1.4f;
    if (mainScale < 2.0f) mainScale = 2.0f;
    if (mainScale > 5.0f) mainScale = 5.0f;
#else
    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    if (primaryMonitor) {
        mainScale = ImGui_ImplGlfw_GetContentScaleForMonitor(primaryMonitor);
    }
#endif
    uiDpiScale = mainScale;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
#if !MODULARITY_RUNTIME_ONLY
    // Interface sounds: widgets report interactions, flushWidgetFeedbackSounds() plays them.
    // Never in the player, where the ImGui on screen is the game's own UI.
    if (!playerMode) {
        ImGui::SetWidgetFeedbackCallback(&Engine::onWidgetFeedback, this);
    }
#endif

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Runtime dock containers use the same ImGui docking support.
#if defined(__ANDROID__)
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
#endif
    if (usingVulkan() || playerMode) {
        io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    } else {
    #ifndef __linux__
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    #endif
    }
    io.IniFilename = nullptr;

    applyModernTheme();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(mainScale);
    style.FontScaleDpi = mainScale;

    {
        ImGuiShadeTheme shade = ImGui::GetShadeTheme();
        shade.Scale = mainScale;
        ImGui::SetShadeTheme(shade);
    }

    initUIStylePresets();

    if (usingVulkan()) {
        if (!ImGui_ImplGlfw_InitForVulkan(editorWindow, true)) {
            throw std::runtime_error("ImGui GLFW Vulkan init failed");
        }
    } else {
        if (!ImGui_ImplGlfw_InitForOpenGL(editorWindow, true)) {
            throw std::runtime_error("ImGui GLFW OpenGL init failed");
        }

        if (!ImGui_ImplOpenGL3_Init(Modularity::OpenGLImGuiGlslVersion())) {
            throw std::runtime_error("ImGui error");
        }

        if (projectManager.preferences.effectiveTier() !=
            Modularity::HardwareProfile::Tier::Low) {
            Modularity::UiGlassBlur::Install();
        }
    }
}
#pragma endregion

void Engine::initUIStylePresets() {
    uiStylePresets.clear();
    uiStylePresets.shrink_to_fit();
    const auto __fcStart = std::chrono::steady_clock::now();
    refreshUIFontCatalog();
    const double __fcMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - __fcStart).count();
    uiEditorFontAsset = getDefaultEditorUIFontAsset();
    applyEditorUIFontById(uiEditorFontAsset);
    const double __faMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - __fcStart).count();
    if (__faMs > Modularity::HardwareProfile::TimingBudget(100.0)) {
        std::fprintf(stderr, "[ModuTimer] setupImGui/refreshUIFontCatalog %.1f ms, +applyFont %.1f ms\n",
                     __fcMs, __faMs - __fcMs);
    }

    UIStylePreset current;
    current.name = "Default";
    current.style = ImGui::GetStyle();
    applyModularityShadeTheme(current.shade);
    current.fontAsset = uiEditorFontAsset;
    current.builtin = true;
    uiStylePresets.push_back(current);

    UIStylePreset glass;
    glass.name = "Glass";
    glass.style = ImGui::GetStyle();
    applyGlassStyle(glass.style);
    glass.fontAsset = uiEditorFontAsset;
    glass.builtin = true;
    uiStylePresets.push_back(glass);

    UIStylePreset slate;
    slate.name = "Slate";
    slate.style = ImGui::GetStyle();
    applySlateStyle(slate.style);
    slate.fontAsset = uiEditorFontAsset;
    slate.builtin = true;
    uiStylePresets.push_back(slate);

    UIStylePreset imguiDefault;
    imguiDefault.name = "Imgui Default";
    imguiDefault.style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&imguiDefault.style);
    applyEditorLayoutPreset(imguiDefault.style);
    imguiDefault.style.GlassBlur = false;
    imguiDefault.style.CheckboxSwitch = false;
    imguiDefault.fontAsset = uiEditorFontAsset;
    imguiDefault.builtin = true;
    uiStylePresets.push_back(imguiDefault);

    UIStylePreset pixel;
    pixel.name = "Pixel";
    pixel.style = ImGui::GetStyle();
    applySlateStyle(pixel.style);
    applyPixelStyle(pixel.style);
    pixel.fontAsset = uiEditorFontAsset;
    pixel.builtin = true;
    uiStylePresets.push_back(pixel);

    UIStylePreset superRound;
    superRound.name = "Super Round";
    superRound.style = ImGui::GetStyle();
    applySlateStyle(superRound.style);
    applySuperRoundStyle(superRound.style);
    superRound.style.CheckboxSwitch = true;
    superRound.fontAsset = uiEditorFontAsset;
    superRound.builtin = true;
    uiStylePresets.push_back(superRound);

    uiStylePresetIndex = findUIStylePreset(uiStylePresetName);
    if (uiStylePresetIndex < 0) {
        uiStylePresetIndex = 0;
        uiStylePresetName = uiStylePresets[0].name;
    }
}

int Engine::findUIStylePreset(const std::string& name) const {
    for (size_t i = 0; i < uiStylePresets.size(); ++i) {
        if (uiStylePresets[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int Engine::findUIFontCatalogIndex(const std::string& id) const {
    for (size_t i = 0; i < uiFontCatalog.size(); ++i) {
        if (uiFontCatalog[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const Engine::UIStylePreset* Engine::getUIStylePreset(const std::string& name) const {
    int idx = findUIStylePreset(name);
    if (idx < 0) return nullptr;
    return &uiStylePresets[idx];
}

std::string Engine::getDefaultEditorUIFontAsset() const {
    const std::array<std::string, 3> preferred = {
        (fs::path("Resources") / "Fonts" / "TheSunset.ttf").generic_string(),
        (fs::path("Resources") / "Fonts" / "Thesunsethd-Regular (1).ttf").generic_string(),
        std::string(kDefaultUIFontAssetId)
    };
    for (const std::string& id : preferred) {
        if (id == kDefaultUIFontAssetId || findUIFontCatalogIndex(id) >= 0) {
            return id;
        }
    }
    return kDefaultUIFontAssetId;
}

fs::path Engine::resolveUIFontPath(const std::string& id) const {
    if (id.empty() || id == kDefaultUIFontAssetId) {
        return fs::path();
    }

    fs::path stored(id);
    if (stored.is_absolute()) {
        return stored;
    }

    std::error_code ec;
    if (fs::exists(stored, ec) && !ec) {
        return stored;
    }

    if (projectManager.currentProject.isLoaded) {
        fs::path candidate = projectManager.currentProject.projectPath / stored;
        if (fs::exists(candidate, ec) && !ec) {
            return candidate;
        }
        candidate = projectManager.currentProject.assetsPath / stored;
        if (fs::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }

    return stored;
}

void Engine::refreshUIFontCatalog() {
    uiFontCatalog.clear();

    auto addEntry = [&](const std::string& id,
                        const std::string& label,
                        const fs::path& path,
                        bool builtin,
                        bool isDefault) {
        if (id.empty() || findUIFontCatalogIndex(id) >= 0) {
            return;
        }
        UIFontCatalogEntry entry;
        entry.id = id;
        entry.label = label;
        entry.path = path;
        entry.builtin = builtin;
        entry.isDefault = isDefault;
        uiFontCatalog.push_back(entry);
    };

    addEntry(kDefaultUIFontAssetId, "ImGui Default", fs::path(), true, true);

    const std::array<fs::path, 2> builtinCandidates = {
        fs::path("Resources") / "Fonts" / "TheSunset.ttf",
        fs::path("Resources") / "Fonts" / "Thesunsethd-Regular (1).ttf"
    };
    for (const fs::path& fontPath : builtinCandidates) {
        if (!Modularity::Platform::GetAssetSource().Exists(fontPath.generic_string())) {
            continue;
        }
        addEntry(fontPath.generic_string(),
                 fontPath.filename().string() + " [Built-in]",
                 fontPath,
                 true,
                 false);
    }

    if (projectManager.currentProject.isLoaded) {
        std::vector<UIFontCatalogEntry> projectEntries;
        const fs::path scanRoot = fs::exists(projectManager.currentProject.assetsPath)
            ? projectManager.currentProject.assetsPath
            : projectManager.currentProject.projectPath;
        std::error_code ec;
        if (fs::exists(scanRoot, ec) && !ec) {
            for (fs::recursive_directory_iterator it(scanRoot, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end;
                 it.increment(ec)) {
                if (ec || !it->is_regular_file()) {
                    continue;
                }
                const fs::path path = it->path();
                if (!IsFontFileExtension(path)) {
                    continue;
                }

                fs::path storedId = fs::relative(path, projectManager.currentProject.projectPath, ec);
                if (ec || storedId.empty()) {
                    storedId = path;
                    ec.clear();
                }

                UIFontCatalogEntry entry;
                entry.id = storedId.generic_string();
                entry.label = path.filename().string() + " [Project]";
                entry.path = path;
                projectEntries.push_back(entry);
            }
        }

        std::sort(projectEntries.begin(), projectEntries.end(), [](const UIFontCatalogEntry& a, const UIFontCatalogEntry& b) {
            if (a.label != b.label) {
                return a.label < b.label;
            }
            return a.id < b.id;
        });
        for (const UIFontCatalogEntry& entry : projectEntries) {
            addEntry(entry.id, entry.label, entry.path, false, false);
        }
    }

    preloadUIFontCatalogForContext(ImGui::GetCurrentContext());
    if (findUIFontCatalogIndex(uiEditorFontAsset) < 0) {
        uiEditorFontAsset = getDefaultEditorUIFontAsset();
    }
}

void Engine::preloadUIFontCatalogForContext(ImGuiContext* context) {
    if (!context) {
        return;
    }

    ImGuiContext* previousContext = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    UIFontContextState& state = uiFontContexts[context];

    if (state.loadedFonts.find(kDefaultUIFontAssetId) == state.loadedFonts.end()) {
        ImFont* defaultFont = io.Fonts->AddFontDefault();
        if (!defaultFont && !io.Fonts->Fonts.empty()) {
            defaultFont = io.Fonts->Fonts.back();
        }
        if (defaultFont) {
            mergeModularityEmojiFont(io, kUIFontAtlasBaseSize, nullptr);
            state.loadedFonts[kDefaultUIFontAssetId] = defaultFont;
        }
    }

    for (const UIFontCatalogEntry& entry : uiFontCatalog) {
        if (entry.isDefault || state.loadedFonts.find(entry.id) != state.loadedFonts.end()) {
            continue;
        }
        fs::path fontPath = entry.path.empty() ? resolveUIFontPath(entry.id) : entry.path;
        if (fontPath.empty()) {
            continue;
        }
        std::vector<uint8_t> bytes =
            Modularity::Platform::GetAssetSource().ReadAll(fontPath.generic_string());
        if (bytes.empty()) {
            continue;
        }
        void* owned = IM_ALLOC(bytes.size());
        if (!owned) {
            continue;
        }
        std::memcpy(owned, bytes.data(), bytes.size());
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(
            owned, static_cast<int>(bytes.size()), kUIFontAtlasBaseSize);
        if (font) {
            mergeModularityEmojiFont(io, kUIFontAtlasBaseSize, nullptr);
            state.loadedFonts[entry.id] = font;
        }
    }

    ImGui::SetCurrentContext(previousContext);
}

ImFont* Engine::getUIFontForContext(const std::string& fontAsset, ImGuiContext* context) {
    if (!context) {
        return nullptr;
    }

    const std::string resolvedId = fontAsset.empty() ? std::string(kDefaultUIFontAssetId) : fontAsset;
    auto contextIt = uiFontContexts.find(context);
    if (contextIt != uiFontContexts.end()) {
        auto fontIt = contextIt->second.loadedFonts.find(resolvedId);
        if (fontIt != contextIt->second.loadedFonts.end() && fontIt->second) {
            return fontIt->second;
        }
        auto defaultIt = contextIt->second.loadedFonts.find(kDefaultUIFontAssetId);
        if (defaultIt != contextIt->second.loadedFonts.end()) {
            return defaultIt->second;
        }
    }
    ImGuiContext* previousContext = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    ImFont* fallback = io.FontDefault ? io.FontDefault : (io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0]);
    ImGui::SetCurrentContext(previousContext);
    return fallback;
}

bool Engine::applyEditorUIFontById(const std::string& fontAsset) {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context) {
        return false;
    }
    ImFont* font = getUIFontForContext(fontAsset, context);
    if (!font) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.FontDefault = font;
    uiEditorFontAsset = fontAsset.empty() ? std::string(kDefaultUIFontAssetId) : fontAsset;
    return true;
}

void Engine::registerUIStylePreset(const std::string& name, const ImGuiStyle& style, bool replace) {
    upsertUIStylePreset(name, style, uiEditorFontAsset, replace);
}

void Engine::upsertUIStylePreset(const std::string& name,
                                 const ImGuiStyle& style,
                                 const std::string& fontAsset,
                                 bool replace,
                                 const ImGuiShadeTheme* shade) {
    if (name.empty()) return;
    int idx = findUIStylePreset(name);
    if (idx >= 0) {
        if (replace) {
            uiStylePresets[idx].style = style;
            uiStylePresets[idx].fontAsset = fontAsset.empty() ? std::string(kDefaultUIFontAssetId) : fontAsset;
            if (shade) uiStylePresets[idx].shade = *shade;
        }
        return;
    }
    UIStylePreset preset;
    preset.name = name;
    preset.style = style;
    if (shade) preset.shade = *shade;
    preset.fontAsset = fontAsset.empty() ? std::string(kDefaultUIFontAssetId) : fontAsset;
    preset.builtin = false;
    uiStylePresets.push_back(preset);
}

void Engine::registerUIStylePresetFromScript(const std::string& name, const ImGuiStyle& style, bool replace) {
    registerUIStylePreset(name, style, replace);
}

bool Engine::saveCurrentUIStyleToPreset(const std::string& name, bool replaceExisting) {
    if (name.empty()) {
        return false;
    }
    if (!replaceExisting && findUIStylePreset(name) >= 0) {
        return false;
    }
    const ImGuiShadeTheme currentShade = ImGui::GetShadeTheme();
    upsertUIStylePreset(name, ImGui::GetStyle(), uiEditorFontAsset, replaceExisting, &currentShade);
    uiStylePresetName = name;
    uiStylePresetIndex = findUIStylePreset(name);
    saveEditorUserSettings();
    return uiStylePresetIndex >= 0;
}

bool Engine::applyUIStylePresetByName(const std::string& name) {
    int idx = findUIStylePreset(name);
    if (idx < 0) {
        return false;
    }
    uiStylePresetIndex = idx;
    uiStylePresetName = uiStylePresets[idx].name;
    ImGui::GetStyle() = uiStylePresets[idx].style;
    if (uiDpiScale > 0.0f && uiDpiScale != 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(uiDpiScale);
    }
    ImGui::GetStyle().FontScaleDpi = (uiDpiScale > 0.0f) ? uiDpiScale : 1.0f;
    ImGuiShadeTheme shade = uiStylePresets[idx].shade;
    shade.Scale = (uiDpiScale > 0.0f) ? uiDpiScale : 1.0f;
    ImGui::SetShadeTheme(shade);
    applyEditorUIFontById(uiStylePresets[idx].fontAsset);
    return true;
}

fs::path Engine::getEditorUserSettingsPath() const {
    if (!projectManager.currentProject.isLoaded) {
        return fs::path();
    }
    fs::path settingsDir = projectManager.currentProject.projectPath / "ProjectUserSettings";
    return settingsDir / "EditorUI.ini";
}

fs::path Engine::getEditorLayoutPath() const {
    return getWorkspaceLayoutPath(WorkspaceMode::Default);
}

fs::path Engine::getWorkspaceLayoutPath(WorkspaceMode mode) const {
    const bool vulkanLayout = usingVulkan();
    const char* filename = vulkanLayout ? "imgui_vulkan.ini" : "imgui.ini";
    if (mode == WorkspaceMode::Animation) {
        filename = vulkanLayout ? "anim_vulkan.ini" : "anim.ini";
    } else if (mode == WorkspaceMode::Scripting) {
        filename = vulkanLayout ? "scripter_vulkan.ini" : "scripter.ini";
    }

    if (projectManager.currentProject.isLoaded) {
        fs::path settingsDir =
            projectManager.currentProject.projectPath / "ProjectUserSettings" / "ProjectLayout";
        return settingsDir / filename;
    }

    return fs::path("Resources") / filename;
}

void Engine::saveWorkspaceLayout(WorkspaceMode mode) const {
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    if (showLauncher || !projectManager.currentProject.isLoaded) {
        return;
    }
    if (pendingWorkspaceReload || workspaceLayoutDirty) {
        return;
    }
    if (mainDockspaceId == 0 || ImGui::DockBuilderGetNode(mainDockspaceId) == nullptr) {
        return;
    }

    fs::path layoutPath = getWorkspaceLayoutPath(mode);
    if (layoutPath.empty()) {
        return;
    }

    std::error_code ec;
    fs::create_directories(layoutPath.parent_path(), ec);
    ImGui::SaveIniSettingsToDisk(layoutPath.string().c_str());
}

void Engine::autosaveWorkspaceLayout() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context || showLauncher || playerMode) {
        return;
    }
    if (!projectManager.currentProject.isLoaded) {
        return;
    }
    if (pendingWorkspaceReload || workspaceLayoutDirty) {
        return;
    }

    if (glfwGetTime() < workspaceLayoutStabilizeUntil) {
        return;
    }

    if (workspaceLayoutAutoRepairPending &&
        mainDockspaceId != 0 &&
        ImGui::DockBuilderGetNode(mainDockspaceId) != nullptr) {
        int trackedWindows = 0;
        int undockedWindows = 0;
        auto countDockState = [&](const char* name, bool shouldBeVisible) {
            if (!shouldBeVisible || !name) {
                return;
            }
            ImGuiWindow* window = ImGui::FindWindowByName(name);
            if (!window) {
                return;
            }
            ++trackedWindows;
            if (window->DockId == 0 || ImGui::DockBuilderGetNode(window->DockId) == nullptr) {
                ++undockedWindows;
            }
        };

        countDockState("Hierarchy", showHierarchy);
        countDockState("Inspector", showInspector);
        countDockState("Project", showFileBrowser);
        countDockState("Console", showConsole);
        countDockState("Viewport", true);
        countDockState("Game Viewport", showGameViewport);
        countDockState("Environment", showEnvironmentWindow);
        countDockState("Camera", showCameraWindow);
        countDockState("Animation", showAnimationWindow);
        countDockState("AI Pathfinding", showAIPathfindingWindow);
        countDockState("Pixel Sprite Editor", showPixelSpriteEditorWindow);
        countDockState("Scripting", showScriptingWindow);
        countDockState("Project Settings", showProjectBrowser);
        countDockState("Modupak Manager", showRegistryPackagesWindow);

        if (trackedWindows >= 4) {
            if (undockedWindows >= 2) {
                buildWorkspaceLayout(currentWorkspace);
                workspaceLayoutSavePending = true;
                workspaceLayoutStabilizeUntil = glfwGetTime() + 0.75;
            }
            workspaceLayoutAutoRepairPending = false;
        }
    }

    if (context->SettingsDirtyTimer > 0.0f) {
        workspaceLayoutSavePending = true;
        return;
    }

    if (workspaceLayoutSavePending) {
        saveWorkspaceLayout(currentWorkspace);
        workspaceLayoutSavePending = false;

        auto layoutFileHasDockNodesForDockspace = [](const fs::path& path, ImGuiID dockspaceId) -> bool {
            if (dockspaceId == 0) {
                return false;
            }
            std::ifstream in(path);
            if (!in.is_open()) {
                return false;
            }
            char dockspaceIdHex[16];
            std::snprintf(dockspaceIdHex, sizeof(dockspaceIdHex), "0x%08X", dockspaceId);
            bool hasDockingData = false;
            bool hasDockNodes = false;
            bool hasMatchingDockspace = false;
            std::string line;
            while (std::getline(in, line)) {
                if (line == "[Docking][Data]") {
                    hasDockingData = true;
                    continue;
                }
                if (!hasDockingData) {
                    continue;
                }
                if (!line.empty() && line.front() == '[') {
                    break;
                }
                if (line.find("DockNode") != std::string::npos) {
                    hasDockNodes = true;
                }
                if (line.find("DockSpace") != std::string::npos &&
                    line.find(dockspaceIdHex) != std::string::npos) {
                    hasMatchingDockspace = true;
                }
            }
            return hasDockingData && hasDockNodes && hasMatchingDockspace;
        };

        fs::path layoutPath = getWorkspaceLayoutPath(currentWorkspace);
        if (!layoutPath.empty() &&
            fs::exists(layoutPath) &&
            !layoutFileHasDockNodesForDockspace(layoutPath, mainDockspaceId) &&
            mainDockspaceId != 0) {
            buildWorkspaceLayout(currentWorkspace);
            saveWorkspaceLayout(currentWorkspace);
        }
    }
}

void Engine::loadEditorUserSettings() {
    if (!projectManager.currentProject.isLoaded) {
        return;
    }
    refreshUIFontCatalog();
    fs::path settingsPath = getEditorUserSettingsPath();
    if (settingsPath.empty() || !fs::exists(settingsPath)) {
        return;
    }

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        if (start == std::string::npos || end == std::string::npos) {
            s.clear();
            return;
        }
        s = s.substr(start, end - start + 1);
    };

    loadedWorkspaceLayoutVersion = 0;
    fileBrowserFavorites.clear();
    workspaceTabVisible = { true, true, true };
    int loadedThemeVersion = 1;
    std::vector<ImVec4> loadedColors(ImGuiCol_COUNT);
    std::vector<bool> hasColor(ImGuiCol_COUNT, false);
    struct LoadedStylePresetEntry {
        std::string name;
        std::string fontAsset = kDefaultUIFontAssetId;
        ImGuiStyle style = ImGui::GetStyle();
        ImGuiShadeTheme shade;
        int shadeVersion = 0;
        bool builtin = false;
        bool hasStyle = false;
        bool hasShade = false;
    };
    std::unordered_map<int, LoadedStylePresetEntry> loadedPresets;
    static std::unordered_map<std::string, int> colorIndex;
    if (colorIndex.empty()) {
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            colorIndex.emplace(ImGui::GetStyleColorName(i), i);
        }
    }

    std::ifstream file(settingsPath);
    std::string line;
    while (std::getline(file, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key);
        trim(value);
        auto applySettingsGroup1 = [&](const std::string& key, const std::string& value) -> bool {
        if (key == "uiStyle") {
            uiStylePresetName = value;
        } else if (key == "uiEditorFont") {
            uiEditorFontAsset = value.empty() ? std::string(kDefaultUIFontAssetId) : value;
        } else if (key == "uiAnimationMode") {
            if (value == "Fluid") {
                uiAnimationMode = UIAnimationMode::Fluid;
            } else if (value == "Snappy") {
                uiAnimationMode = UIAnimationMode::Snappy;
            } else {
                uiAnimationMode = UIAnimationMode::Off;
            }
        } else if (key == "gameTimeScale") {
            try {
                gameTimeScale = std::clamp(std::stof(value), kMinGameTimeScale, kMaxGameTimeScale);
            } catch (...) {
                gameTimeScale = 1.0f;
            }
        } else if (key == "uiChromeScale") {
            if (value == "Big") {
                uiChromeScale = EditorChromeScale::Big;
            } else if (value == "Compact") {
                uiChromeScale = EditorChromeScale::Compact;
            } else {
                uiChromeScale = EditorChromeScale::Default;
            }
        } else if (key == "workspace") {
            if (value == "Animation") {
                currentWorkspace = WorkspaceMode::Animation;
            } else if (value == "Scripting") {
                currentWorkspace = WorkspaceMode::Scripting;
            } else {
                currentWorkspace = WorkspaceMode::Default;
            }
        } else if (key == "workspaceLayoutVersion") {
            try {
                loadedWorkspaceLayoutVersion = std::max(0, std::stoi(value));
            } catch (...) {
                loadedWorkspaceLayoutVersion = 0;
            }
        } else if (key == "workspaceTab.Default") {
            workspaceTabVisible[0] = (value == "1" || value == "true" || value == "yes");
        } else if (key == "workspaceTab.Animation") {
            workspaceTabVisible[1] = (value == "1" || value == "true" || value == "yes");
        } else if (key == "workspaceTab.Scripting") {
            workspaceTabVisible[2] = (value == "1" || value == "true" || value == "yes");
        } else if (key == "fileBrowserIconScale") {
            try {
                fileBrowserIconScale = std::stof(value);
            } catch (...) {
            }
        } else if (key == "fileBrowserViewMode") {
            if (value == "List") {
                fileBrowser.viewMode = FileBrowserViewMode::List;
            } else {
                fileBrowser.viewMode = FileBrowserViewMode::Grid;
            }
        } else if (key == "fileBrowserSidebarWidth") {
            try {
                fileBrowserSidebarWidth = std::stof(value);
            } catch (...) {
            }
        } else if (key == "fileBrowserSidebarVisible") {
            showFileBrowserSidebar = (value == "1" || value == "true" || value == "yes");
        } else if (key == "consoleWrapText") {
            consoleWrapText = (value == "1" || value == "true" || value == "yes");
        } else if (key == "playModeSaveChoice") {
            if (value == "SaveAnyway") {
                playModeSaveChoice = PlayModeSaveChoice::SaveAnyway;
            } else if (value == "ExitPlayMode") {
                playModeSaveChoice = PlayModeSaveChoice::ExitPlayMode;
            } else {
                playModeSaveChoice = PlayModeSaveChoice::Ask;
            }
        } else if (key == "showTouchSticks") {
            showTouchSticks = (value == "1" || value == "true" || value == "yes");
        } else if (key == "touchStickRadius") {
            try {
                touchStickRadius = std::clamp(std::stof(value), 24.0f, 96.0f);
            } catch (...) {
            }
        } else if (key == "touchStickSensitivity") {
            try {
                touchStickSensitivity = std::clamp(std::stof(value), 1.0f, 16.0f);
            } catch (...) {
            }
        } else if (key == "touchStickInvertY") {
            touchStickInvertY = (value == "1" || value == "true" || value == "yes");
        } else if (key == "quickToolsPinned") {
            quickToolsPinned = (value == "1" || value == "true" || value == "yes");
        } else if (key == "mobileEditorLayout") {
            mobileEditorLayout = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showAnimationWindow") {
            showAnimationWindow = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showAIPathfindingWindow") {
            showAIPathfindingWindow = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showPixelSpriteEditorWindow") {
            showPixelSpriteEditorWindow = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showProjectBrowser") {
            showProjectBrowser = (value == "1" || value == "true" || value == "yes");
        } else if (key == "projectSettingsCompactSidebar") {
            projectSettingsCompactSidebar = (value == "1" || value == "true" || value == "yes");
        } else if (key == "projectSettingsTwoColumns") {
            projectSettingsTwoColumns = (value == "1" || value == "true" || value == "yes");
        } else if (key == "projectSettingsVisibilityMode") {
            try { projectSettingsVisibilityMode = std::clamp(std::stoi(value), 0, 2); } catch (...) {}
        } else if (key == "showRegistryPackagesWindow") {
            showRegistryPackagesWindow = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showGameProfilerWindow") {
            showGameProfilerWindow = (value == "1" || value == "true" || value == "yes");
        } else if (key == "hierarchyShowTexturePreview") {
            hierarchyShowTexturePreview = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showSceneGizmos") {
            showSceneGizmos = (value == "1" || value == "true" || value == "yes");
        } else if (key == "gizmoShowCameraOverlays") {
            gizmoShowCameraOverlays = (value == "1" || value == "true" || value == "yes");
        } else if (key == "gizmoShowCameraFrustumLabels") {
            gizmoShowCameraFrustumLabels = (value == "1" || value == "true" || value == "yes");
        } else {
            return false;
        }
        return true;
        };
        auto applySettingsGroup2 = [&](const std::string& key, const std::string& value) -> bool {
        if (key == "gizmoShowLightOverlays") {
            gizmoShowLightOverlays = (value == "1" || value == "true" || value == "yes");
        } else if (key == "gizmoShowLightIntensityLabels") {
            gizmoShowLightIntensityLabels = (value == "1" || value == "true" || value == "yes");
        } else if (key == "gizmoShowInvariableLocks") {
            gizmoShowInvariableLocks = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showViewportHintOverlay") {
            showViewportHintOverlay = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showLight2DStatsOverlay") {
            showLight2DStatsOverlay = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showLightmappingWindow") {
            showLightmappingWindow = (value == "1" || value == "true" || value == "yes");
        } else if (key == "blenderExecutablePath") {
            blenderExecutableOverride = value;
        } else if (key == "showViewport") {
            showViewport = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showGameViewport") {
            showGameViewport = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showXRPreviewWindow") {
            showXRPreviewWindow = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showVersionControlWindow") {
            showVersionControlWindow = (value == "1" || value == "true" || value == "yes");
        } else if (key == "xrPreviewPreset") {
            xrPreview.presetIndex = std::clamp(std::atoi(value.c_str()), 0, 5);
        } else if (key == "xrPreviewRenderScale") {
            xrPreview.renderScale = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.4f, 2.0f);
        } else if (key == "xrPreviewTargetHz") {
            xrPreview.targetHz = std::clamp(static_cast<float>(std::atof(value.c_str())), 30.0f, 240.0f);
        } else if (key == "xrPreviewIpdMm") {
            xrPreview.ipdMm = std::clamp(static_cast<float>(std::atof(value.c_str())), 50.0f, 76.0f);
        } else if (key == "xrPreviewFov") {
            xrPreview.fovDegrees = std::clamp(static_cast<float>(std::atof(value.c_str())), 60.0f, 130.0f);
#if !MODULARITY_RUNTIME_ONLY
        } else if (key == "lightmapQuality") {
            try {
                lightmapping.settings.quality = static_cast<Nebula::LightmapQuality>(
                    std::clamp(std::stoi(value), 0, 2));
            } catch (...) {}
        } else if (key == "lightmapMode") {
            try {
                lightmapping.settings.mode = static_cast<Nebula::LightmapMode>(
                    std::clamp(std::stoi(value), 0, 2));
            } catch (...) {}
        } else if (key == "lightmapBounces") {
            try { lightmapping.settings.bounces = std::clamp(std::stoi(value), 0, 4); } catch (...) {}
        } else if (key == "lightmapFinalGatherRays") {
            try {
                lightmapping.settings.finalGatherRays = std::clamp(std::stoi(value), 1, 8192);
            } catch (...) {}
        } else if (key == "lightmapResolution") {
            try {
                lightmapping.settings.resolution = std::clamp(std::stof(value), 0.1f, 4096.0f);
            } catch (...) {}
        } else if (key == "lightmapSkyIntensity") {
            try {
                lightmapping.settings.skyLightIntensity = std::clamp(std::stof(value), 0.0f, 32.0f);
            } catch (...) {}
        } else if (key == "lightmapInterpolation") {
            try {
                lightmapping.settings.interpolation = std::clamp(std::stof(value), 0.0f, 1.0f);
            } catch (...) {}
        } else if (key == "lightmapInterpolationPoints") {
            try {
                lightmapping.settings.interpolationPoints = std::clamp(std::stoi(value), 1, 64);
            } catch (...) {}
        } else if (key == "lightmapLockAtlas") {
            lightmapping.settings.lockAtlas = (value == "1" || value == "true" || value == "yes");
        } else if (key == "lightmapLockedAtlasSize") {
            try {
                lightmapping.settings.lockedAtlasSize = std::clamp(std::stoi(value), 64, 8192);
            } catch (...) {}
#endif
        } else if (key == "sceneViewportRenderMode") {
            try {
                sceneViewportRenderMode = static_cast<SceneRenderMode>(
                    std::clamp(std::stoi(value), 0, 2));
            } catch (...) {}
        } else if (key == "light2DLightingBufferScale") {
            try {
                buildSettings.light2DLightingResolution = std::clamp(std::stof(value), 0.25f, 1.0f);
                buildSettingsDirty = true;
                saveBuildSettings();
            } catch (...) {}
        } else if (key == "maxRealtimeLights") {
            try { renderer.setMaxRealtimeLights(std::stoi(value)); } catch (...) {}
        } else if (key == "world2DPostFxEnabled") {
            world2DPostFx.enabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "world2DPostFxDitherIntensity") {
            try { world2DPostFx.ditherIntensity = std::clamp(std::stof(value), 0.0f, 1.5f); } catch (...) {}
        } else if (key == "world2DPostFxColorBits") {
            try { world2DPostFx.colorBits = std::clamp(std::stoi(value), 1, 8); } catch (...) {}
        } else if (key == "world2DPostFxDarkAdjustment") {
            try { world2DPostFx.darkAdjustment = std::clamp(std::stof(value), 0.0f, 1.0f); } catch (...) {}
        } else if (key == "world2DPostFxDitherScale") {
            try { world2DPostFx.ditherScale = std::clamp(std::stof(value), 1.0f, 8.0f); } catch (...) {}
        } else if (key == "world2DPostFxPixelation") {
            try { world2DPostFx.pixelation = std::clamp(std::stof(value), 0.0f, 64.0f); } catch (...) {}
        } else if (key == "world2DPostFxExposure") {
            try { world2DPostFx.exposure = std::clamp(std::stof(value), -4.0f, 4.0f); } catch (...) {}
        } else if (key == "world2DPostFxContrast") {
            try { world2DPostFx.contrast = std::clamp(std::stof(value), 0.0f, 2.5f); } catch (...) {}
        } else if (key == "world2DPostFxSaturation") {
            try { world2DPostFx.saturation = std::clamp(std::stof(value), 0.0f, 2.5f); } catch (...) {}
        } else if (key == "world2DPostFxColorFilterR") {
            try { world2DPostFx.colorFilter.r = std::clamp(std::stof(value), 0.0f, 2.0f); } catch (...) {}
        } else if (key == "world2DPostFxColorFilterG") {
            try { world2DPostFx.colorFilter.g = std::clamp(std::stof(value), 0.0f, 2.0f); } catch (...) {}
        } else if (key == "world2DPostFxColorFilterB") {
            try { world2DPostFx.colorFilter.b = std::clamp(std::stof(value), 0.0f, 2.0f); } catch (...) {}
        } else if (key == "world2DPostFxVignetteIntensity") {
            try { world2DPostFx.vignetteIntensity = std::clamp(std::stof(value), 0.0f, 1.0f); } catch (...) {}
        } else if (key == "world2DPostFxVignetteSmoothness") {
            try { world2DPostFx.vignetteSmoothness = std::clamp(std::stof(value), 0.05f, 1.0f); } catch (...) {}
        } else if (key == "world2DPostFxChromaticAmount") {
            try { world2DPostFx.chromaticAmount = std::clamp(std::stof(value), 0.0f, 0.05f); } catch (...) {}
        } else if (key == "world2DPostFxSharpenStrength") {
            try { world2DPostFx.sharpenStrength = std::clamp(std::stof(value), 0.0f, 2.0f); } catch (...) {}
        } else if (key == "world2DPostFxGrainAmount") {
            try { world2DPostFx.grainAmount = std::clamp(std::stof(value), 0.0f, 0.4f); } catch (...) {}
        } else if (key == "world2DPostFxScanlineIntensity") {
            try { world2DPostFx.scanlineIntensity = std::clamp(std::stof(value), 0.0f, 1.0f); } catch (...) {}
        } else if (key == "gizmoShowLight2DBounds") {
            gizmoShowLight2DBounds = (value == "1" || value == "true" || value == "yes");
        } else if (key == "gizmoShowLight2DShapes") {
            gizmoShowLight2DShapes = (value == "1" || value == "true" || value == "yes");
        } else if (key == "gizmoShowShadowCaster2DBounds") {
            gizmoShowShadowCaster2DBounds = (value == "1" || value == "true" || value == "yes");
        } else if (key == "sceneGizmoIconScale") {
            try { sceneGizmoIconScale = std::stof(value); } catch (...) {}
        } else if (key == "sceneGizmoOverlayScale") {
            try { sceneGizmoOverlayScale = std::stof(value); } catch (...) {}
        } else if (key == "showSceneGrid3D") {
            showSceneGrid3D = (value == "1" || value == "true" || value == "yes");
        } else {
            return false;
        }
        return true;
        };
        auto applySettingsGroup3 = [&](const std::string& key, const std::string& value) -> bool {
        if (key == "uiCanvasPreviewEnabled") {
            uiCanvasPreviewEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showCanvasOverlay") {
            showCanvasOverlay = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showUIWorldGrid") {
            showUIWorldGrid = (value == "1" || value == "true" || value == "yes");
        } else if (key == "pixelGridSnapEnabled") {
            pixelGridSnapEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "pixelGridSnapStep") {
            try { pixelGridSnapStep = std::clamp(std::stoi(value), 1, 64); } catch (...) {}
        } else if (key == "tmPresentationPitchStretchEnabled") {
            tmOpenGLRenderer.getPresentationSettings().lookPitchStretchEnabled =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "tmPresentationPitchStretchStrength") {
            try { tmOpenGLRenderer.getPresentationSettings().lookPitchStretchStrength = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationPitchCompressStrength") {
            try { tmOpenGLRenderer.getPresentationSettings().lookPitchCompressStrength = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationPitchShearStrength") {
            try { tmOpenGLRenderer.getPresentationSettings().lookPitchShearStrength = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationPitchCurve") {
            try { tmOpenGLRenderer.getPresentationSettings().lookPitchCurve = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationPitchDepthRange") {
            try { tmOpenGLRenderer.getPresentationSettings().lookPitchDepthRange = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationPitchMinDegrees") {
            try { tmOpenGLRenderer.getPresentationSettings().presentationPitchMinDegrees = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationPitchMaxDegrees") {
            try { tmOpenGLRenderer.getPresentationSettings().presentationPitchMaxDegrees = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationWorldSnapEnabled") {
            tmOpenGLRenderer.getPresentationSettings().presentationSnapEnabled =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "tmPresentationWorldSnapStep") {
            try { tmOpenGLRenderer.getPresentationSettings().presentationSnapStep = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationCameraSnapEnabled") {
            tmOpenGLRenderer.getPresentationSettings().cameraRelativeSnapEnabled =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "tmPresentationCameraSnapStep") {
            try { tmOpenGLRenderer.getPresentationSettings().cameraRelativeSnapStep = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationVertexSnapEnabled") {
            tmOpenGLRenderer.getPresentationSettings().vertexSnapEnabled =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "tmPresentationVertexSnapStep") {
            try { tmOpenGLRenderer.getPresentationSettings().vertexSnapStep = std::stof(value); } catch (...) {}
        } else if (key == "tmPresentationScreenSnapEnabled") {
            tmOpenGLRenderer.getPresentationSettings().screenSnapEnabled =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "tmPresentationScreenSnapStep") {
            try { tmOpenGLRenderer.getPresentationSettings().screenSnapStep = std::stof(value); } catch (...) {}
        } else if (key == "tmFake3DEnabled") {
            tmOpenGLRenderer.getPresentationSettings().fake3DEnabled =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "tmFake3DInternalHeight") {
            try { tmOpenGLRenderer.getPresentationSettings().fake3DInternalHeight = std::stoi(value); } catch (...) {}
        } else if (key == "tmFake3DPointSampling") {
            tmOpenGLRenderer.getPresentationSettings().fake3DPointSampling =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "tmFake3DFlatShading") {
            tmOpenGLRenderer.getPresentationSettings().fake3DFlatShading =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "tmFake3DShadeLevels") {
            try { tmOpenGLRenderer.getPresentationSettings().fake3DShadeLevels = std::stoi(value); } catch (...) {}
        } else if (key == "tmFake3DAffineTextures") {
            tmOpenGLRenderer.getPresentationSettings().fake3DAffineTextures =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "tmFake3DAffineStrength") {
            try { tmOpenGLRenderer.getPresentationSettings().fake3DAffineStrength = std::stof(value); } catch (...) {}
        } else if (key == "showGameProfiler") {
            showGameProfiler = (value == "1" || value == "true" || value == "yes");
        } else if (key == "revealDebugSectionsAndMenus") {
            revealDebugSectionsAndMenus = (value == "1" || value == "true" || value == "yes");
        } else if (key == "showGameProfilerWindow") {
            revealDebugSectionsAndMenus = (value == "1" || value == "true" || value == "yes");
        } else {
            return false;
        }
        return true;
        };
        auto applySettingsGroup4 = [&](const std::string& key, const std::string& value) -> bool {
        if (key == "collisionWireframe") {
            collisionWireframe = (value == "1" || value == "true" || value == "yes");
        } else if (key == "fpsCapEnabled") {
            fpsCapEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "fpsCap") {
            try { fpsCap = std::max(1.0f, std::stof(value)); } catch (...) {}
        } else if (key == "editorVSyncEnabled") {
            editorVSyncEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "cameraMoveSpeed") {
            try { camera.moveSpeed = std::max(0.01f, std::stof(value)); } catch (...) {}
        } else if (key == "cameraSprintSpeed") {
            try { camera.sprintSpeed = std::max(camera.moveSpeed, std::stof(value)); } catch (...) {}
        } else if (key == "cameraSmoothMovement") {
            camera.smoothMovement = (value == "1" || value == "true" || value == "yes");
        } else if (key == "cameraAcceleration") {
            try { camera.acceleration = std::max(0.1f, std::stof(value)); } catch (...) {}
        } else if (key == "cameraMouseSensitivity") {
            try { camera.mouseSensitivity = std::max(0.001f, std::stof(value)); } catch (...) {}
        } else if (key == "gameViewportResolutionIndex") {
            try { gameViewportResolutionIndex = std::max(0, std::stoi(value)); } catch (...) {}
        } else if (key == "gameViewportCustomWidth") {
            try { gameViewportCustomWidth = std::clamp(std::stoi(value), 64, 8192); } catch (...) {}
        } else if (key == "gameViewportCustomHeight") {
            try { gameViewportCustomHeight = std::clamp(std::stoi(value), 64, 8192); } catch (...) {}
        } else if (key == "gameViewportAutoFit") {
            gameViewportAutoFit = (value == "1" || value == "true" || value == "yes");
        } else if (key == "gameViewportZoom") {
            try { gameViewportZoom = std::clamp(std::stof(value), 1.0f, 8.0f); } catch (...) {}
        } else if (key == "sceneViewportRenderWidth") {
            try { sceneViewportRenderWidth = std::clamp(std::stoi(value), 64, 8192); } catch (...) {}
        } else if (key == "sceneViewportRenderHeight") {
            try { sceneViewportRenderHeight = std::clamp(std::stoi(value), 64, 8192); } catch (...) {}
        } else if (key == "sceneViewportDisplayMode") {
            if (value == "Fit") {
                sceneViewportDisplayMode = ViewportDisplayMode::Fit;
            } else if (value == "Fill") {
                sceneViewportDisplayMode = ViewportDisplayMode::Fill;
            } else if (value == "IntegerScale") {
                sceneViewportDisplayMode = ViewportDisplayMode::IntegerScale;
            } else {
                sceneViewportDisplayMode = ViewportDisplayMode::Stretch;
            }
        } else if (key == "sceneViewportToolbarCorner") {
            if (value == "BottomRight") {
                sceneViewportToolbarCorner = ViewportToolbarCorner::BottomRight;
            } else if (value == "TopLeft") {
                sceneViewportToolbarCorner = ViewportToolbarCorner::TopLeft;
            } else if (value == "TopRight") {
                sceneViewportToolbarCorner = ViewportToolbarCorner::TopRight;
            } else {
                sceneViewportToolbarCorner = ViewportToolbarCorner::BottomLeft;
            }
        } else if (key == "gameViewportDisplayMode") {
            if (value == "Stretch") {
                gameViewportDisplayMode = ViewportDisplayMode::Stretch;
            } else if (value == "Fill") {
                gameViewportDisplayMode = ViewportDisplayMode::Fill;
            } else if (value == "IntegerScale") {
                gameViewportDisplayMode = ViewportDisplayMode::IntegerScale;
            } else {
                gameViewportDisplayMode = ViewportDisplayMode::Fit;
            }
        } else if (key == "scriptAutoCompileInterval") {
            try { scriptAutoCompileInterval = std::clamp(std::stod(value), 0.1, 10.0); } catch (...) {}
        } else if (key == "scriptAutoCompileOnSave") {
            scriptEditorState.autoCompileOnSave = (value == "1" || value == "true" || value == "yes");
        } else if (key == "audioPreviewVolume") {
            try { audioPreviewVolume = std::stof(value); } catch (...) {}
        } else if (key == "feedbackSoundsEnabled") {
            feedbackSoundsEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "feedbackClickSoundsEnabled") {
            feedbackClickSoundsEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "feedbackErrorSoundsEnabled") {
            feedbackErrorSoundsEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "feedbackOtherSoundsEnabled") {
            feedbackOtherSoundsEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "feedbackButtonSoundsEnabled") {
            feedbackButtonSoundsEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "feedbackToggleSoundsEnabled") {
            feedbackToggleSoundsEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "feedbackSlideSoundsEnabled") {
            feedbackSlideSoundsEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "feedbackTransitionSoundsEnabled") {
            feedbackTransitionSoundsEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "feedbackHoverSoundsEnabled") {
            feedbackHoverSoundsEnabled = (value == "1" || value == "true" || value == "yes");
        } else if (key == "feedbackSoundVolume") {
            try { feedbackSoundVolume = std::clamp(std::stof(value), 0.0f, 1.0f); } catch (...) {}
        } else if (key == "themeVersion") {
            try { loadedThemeVersion = std::stoi(value); } catch (...) {}
        } else if (key.rfind("stylePreset", 0) == 0) {
            size_t indexStart = std::strlen("stylePreset");
            size_t fieldSep = key.find('_', indexStart);
            if (fieldSep != std::string::npos) {
                int presetIndex = -1;
                try {
                    presetIndex = std::stoi(key.substr(indexStart, fieldSep - indexStart));
                } catch (...) {
                    presetIndex = -1;
                }
                if (presetIndex >= 0) {
                    LoadedStylePresetEntry& preset = loadedPresets[presetIndex];
                    const std::string field = key.substr(fieldSep + 1);
                    if (field == "name") {
                        preset.name = value;
                    } else if (field == "font") {
                        preset.fontAsset = value.empty() ? std::string(kDefaultUIFontAssetId) : value;
                    } else if (field == "builtin") {
                        preset.builtin = (value == "1" || value == "true" || value == "yes");
                    } else if (field == "styleBlob") {
                        ImGuiStyle decodedStyle = ImGui::GetStyle();
                        if (DecodeStyleBlob(value, decodedStyle)) {
                            preset.style = decodedStyle;
                            preset.hasStyle = true;
                        }
                    } else if (ParseShadeThemeField(field, value, preset.shade, &preset.shadeVersion)) {
                        preset.hasShade = true;
                    }
                }
            }
        } else if (key.rfind("color.", 0) == 0) {
            std::string name = key.substr(6);
            auto it = colorIndex.find(name);
            if (it != colorIndex.end()) {
                std::string parseValue = value;
                std::replace(parseValue.begin(), parseValue.end(), ',', ' ');
                std::stringstream ss(parseValue);
                float r = 0.0f;
                float g = 0.0f;
                float b = 0.0f;
                float a = 1.0f;
                if (ss >> r >> g >> b >> a) {
                    loadedColors[it->second] = ImVec4(r, g, b, a);
                    hasColor[it->second] = true;
                }
            }
        } else if (key == "favorite") {
            if (value.empty()) {
                return true;
            }
            fs::path favPath = fs::path(value);
            fs::path baseRoot = fileBrowser.projectRoot.empty()
                ? projectManager.currentProject.projectPath
                : fileBrowser.projectRoot;
            if (favPath.is_relative()) {
                favPath = baseRoot / favPath;
            }
            std::error_code ec;
            fs::path canonical = fs::weakly_canonical(favPath, ec);
            if (!ec) {
                favPath = canonical;
            }
            fileBrowserFavorites.push_back(favPath);
        } else {
            return false;
        }
        return true;
        };

        if (applySettingsGroup1(key, value)) continue;
        if (applySettingsGroup2(key, value)) continue;
        if (applySettingsGroup3(key, value)) continue;
        applySettingsGroup4(key, value);
    }

    fileBrowserIconScale = std::clamp(fileBrowserIconScale, 0.6f, 2.0f);
    fileBrowserSidebarWidth = std::clamp(fileBrowserSidebarWidth, 160.0f, 360.0f);
    sceneGizmoIconScale = std::clamp(sceneGizmoIconScale, 0.4f, 3.0f);
    sceneGizmoOverlayScale = std::clamp(sceneGizmoOverlayScale, 0.4f, 3.0f);
    sceneViewportRenderMode = static_cast<SceneRenderMode>(
        std::clamp(static_cast<int>(sceneViewportRenderMode), 0, 2));
    camera.moveSpeed = std::max(0.01f, camera.moveSpeed);
    camera.sprintSpeed = std::max(camera.moveSpeed, camera.sprintSpeed);
    camera.acceleration = std::max(0.1f, camera.acceleration);
    camera.mouseSensitivity = std::clamp(camera.mouseSensitivity, 0.001f, 2.0f);
    fpsCap = std::max(1.0f, fpsCap);
    gameViewportCustomWidth = std::clamp(gameViewportCustomWidth, 64, 8192);
    gameViewportCustomHeight = std::clamp(gameViewportCustomHeight, 64, 8192);
    gameViewportZoom = std::clamp(gameViewportZoom, 1.0f, 8.0f);
    sceneViewportRenderWidth = std::clamp(sceneViewportRenderWidth, 64, 8192);
    sceneViewportRenderHeight = std::clamp(sceneViewportRenderHeight, 64, 8192);
    buildSettings.light2DLightingResolution = std::clamp(buildSettings.light2DLightingResolution, 0.25f, 1.0f);
    renderer.setMaxRealtimeLights(renderer.getMaxRealtimeLights());
    pixelGridSnapStep = std::clamp(pixelGridSnapStep, 1, 64);
    {
        auto& tmPresentation = tmOpenGLRenderer.getPresentationSettings();
        tmPresentation.lookPitchStretchStrength = std::clamp(tmPresentation.lookPitchStretchStrength, 0.0f, 1.5f);
        tmPresentation.lookPitchCompressStrength = std::clamp(tmPresentation.lookPitchCompressStrength, 0.0f, 1.5f);
        tmPresentation.lookPitchShearStrength = std::clamp(tmPresentation.lookPitchShearStrength, 0.0f, 1.0f);
        tmPresentation.presentationSnapStep = std::clamp(tmPresentation.presentationSnapStep, 0.001f, 8.0f);
        tmPresentation.cameraRelativeSnapStep = std::clamp(tmPresentation.cameraRelativeSnapStep, 0.001f, 8.0f);
        tmPresentation.vertexSnapStep = std::clamp(tmPresentation.vertexSnapStep, 0.0005f, 4.0f);
        tmPresentation.screenSnapStep = std::clamp(tmPresentation.screenSnapStep, 0.25f, 16.0f);
        tmPresentation.fake3DInternalHeight = std::clamp(tmPresentation.fake3DInternalHeight, 64, 2160);
        tmPresentation.fake3DShadeLevels = std::clamp(tmPresentation.fake3DShadeLevels, 0, 16);
        tmPresentation.fake3DAffineStrength = std::clamp(tmPresentation.fake3DAffineStrength, 0.0f, 1.0f);
        tmPresentation.lookPitchCurve = std::clamp(tmPresentation.lookPitchCurve, 0.1f, 4.0f);
        tmPresentation.lookPitchDepthRange = std::clamp(tmPresentation.lookPitchDepthRange, 1.0f, 256.0f);
        tmPresentation.presentationPitchMinDegrees = std::clamp(tmPresentation.presentationPitchMinDegrees, -89.0f, 89.0f);
        tmPresentation.presentationPitchMaxDegrees = std::clamp(tmPresentation.presentationPitchMaxDegrees, -89.0f, 89.0f);
    }
    scriptAutoCompileInterval = std::clamp(scriptAutoCompileInterval, 0.1, 10.0);
    audioPreviewVolume = std::clamp(audioPreviewVolume, 0.0f, 2.0f);

    if (!workspaceTabVisible[0] && !workspaceTabVisible[1] && !workspaceTabVisible[2]) {
        workspaceTabVisible[0] = true;
    }
    auto workspaceToIndex = [](WorkspaceMode mode) {
        switch (mode) {
            case WorkspaceMode::Default: return 0;
            case WorkspaceMode::Animation: return 1;
            case WorkspaceMode::Scripting: return 2;
        }
        return 0;
    };
    if (!workspaceTabVisible[workspaceToIndex(currentWorkspace)]) {
        if (workspaceTabVisible[0]) {
            currentWorkspace = WorkspaceMode::Default;
        } else if (workspaceTabVisible[1]) {
            currentWorkspace = WorkspaceMode::Animation;
        } else {
            currentWorkspace = WorkspaceMode::Scripting;
        }
    }

    clampOptionalPackageState(false);

    std::vector<int> loadedPresetIndices;
    loadedPresetIndices.reserve(loadedPresets.size());
    for (const auto& entry : loadedPresets) {
        loadedPresetIndices.push_back(entry.first);
    }
    ImGuiShadeTheme legacyPresetShade;
    applyModularityShadeTheme(legacyPresetShade);
    std::sort(loadedPresetIndices.begin(), loadedPresetIndices.end());
    for (int presetId : loadedPresetIndices) {
        const LoadedStylePresetEntry& preset = loadedPresets[presetId];
        if (preset.name.empty()) {
            continue;
        }
        ImGuiShadeTheme repairedShade;
        bool useRepairedShade = false;
        if (preset.hasShade && preset.shade.Enabled &&
            (preset.shadeVersion < kShadeFormatVersion || !ShadeThemeHasVisibleShading(preset.shade))) {
            repairedShade = legacyPresetShade;
            repairedShade.GradientScale = preset.shade.GradientScale;
            repairedShade.BevelScale = preset.shade.BevelScale;
            useRepairedShade = true;
        }
        const ImGuiShadeTheme* presetShade = useRepairedShade ? &repairedShade
                                           : preset.hasShade ? &preset.shade
                                           : preset.builtin ? nullptr
                                           : &legacyPresetShade;
        upsertUIStylePreset(
            preset.name,
            preset.hasStyle ? preset.style : ImGui::GetStyle(),
            preset.fontAsset,
            true,
            presetShade);
        const int idx = findUIStylePreset(preset.name);
        if (idx >= 0) {
            uiStylePresets[idx].builtin = preset.builtin;
        }
    }

    applyUIStylePresetByName(uiStylePresetName);
    ImGuiStyle& style = ImGui::GetStyle();
    if (loadedThemeVersion >= 3) {
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            if (hasColor[i]) {
                style.Colors[i] = loadedColors[i];
            }
        }
    }
    applyEditorUIFontById(uiEditorFontAsset);

    applyWorkspacePreset(currentWorkspace, false);
    scriptingFilesDirty = true;
    applyPresentationSettings();
}

void Engine::saveEditorUserSettings() const {
    if (!projectManager.currentProject.isLoaded) {
        return;
    }
    fs::path settingsPath = getEditorUserSettingsPath();
    if (settingsPath.empty()) {
        return;
    }
    fs::create_directories(settingsPath.parent_path());

    std::ofstream file(settingsPath);
    if (!file.is_open()) {
        return;
    }

    file << "# Editor UI settings\n";
    file << std::fixed << std::setprecision(4);
    file << "themeVersion=3\n";
    file << "uiStyle=" << uiStylePresetName << "\n";
    file << "uiEditorFont=" << uiEditorFontAsset << "\n";
    const char* animMode = "Off";
    if (uiAnimationMode == UIAnimationMode::Fluid) {
        animMode = "Fluid";
    } else if (uiAnimationMode == UIAnimationMode::Snappy) {
        animMode = "Snappy";
    }
    file << "uiAnimationMode=" << animMode << "\n";
    file << "uiChromeScale=" << getEditorChromeScaleLabel(uiChromeScale) << "\n";
    file << "gameTimeScale=" << gameTimeScale << "\n";
    const char* workspaceName = "Default";
    if (currentWorkspace == WorkspaceMode::Animation) {
        workspaceName = "Animation";
    } else if (currentWorkspace == WorkspaceMode::Scripting) {
        workspaceName = "Scripting";
    }
    file << "workspace=" << workspaceName << "\n";
    file << "workspaceLayoutVersion=" << kWorkspaceLayoutVersion << "\n";
    file << "workspaceTab.Default=" << (workspaceTabVisible[0] ? "1" : "0") << "\n";
    file << "workspaceTab.Animation=" << (workspaceTabVisible[1] ? "1" : "0") << "\n";
    file << "workspaceTab.Scripting=" << (workspaceTabVisible[2] ? "1" : "0") << "\n";
    file << "fileBrowserIconScale=" << fileBrowserIconScale << "\n";
    file << "fileBrowserViewMode=" << (fileBrowser.viewMode == FileBrowserViewMode::List ? "List" : "Grid") << "\n";
    file << "fileBrowserSidebarWidth=" << fileBrowserSidebarWidth << "\n";
    file << "fileBrowserSidebarVisible=" << (showFileBrowserSidebar ? "1" : "0") << "\n";
    file << "consoleWrapText=" << (consoleWrapText ? "1" : "0") << "\n";
    const char* playModeSaveChoiceName = "Ask";
    if (playModeSaveChoice == PlayModeSaveChoice::SaveAnyway) {
        playModeSaveChoiceName = "SaveAnyway";
    } else if (playModeSaveChoice == PlayModeSaveChoice::ExitPlayMode) {
        playModeSaveChoiceName = "ExitPlayMode";
    }
    file << "playModeSaveChoice=" << playModeSaveChoiceName << "\n";
    file << "showTouchSticks=" << (showTouchSticks ? "1" : "0") << "\n";
    file << "touchStickRadius=" << std::clamp(touchStickRadius, 24.0f, 96.0f) << "\n";
    file << "touchStickSensitivity=" << std::clamp(touchStickSensitivity, 1.0f, 16.0f) << "\n";
    file << "touchStickInvertY=" << (touchStickInvertY ? "1" : "0") << "\n";
    file << "quickToolsPinned=" << (quickToolsPinned ? "1" : "0") << "\n";
    file << "mobileEditorLayout=" << (mobileEditorLayout ? "1" : "0") << "\n";
    file << "showAnimationWindow=" << (showAnimationWindow ? "1" : "0") << "\n";
    file << "showAIPathfindingWindow=" << (showAIPathfindingWindow ? "1" : "0") << "\n";
    file << "showPixelSpriteEditorWindow=" << (showPixelSpriteEditorWindow ? "1" : "0") << "\n";
    file << "showProjectBrowser=" << (showProjectBrowser ? "1" : "0") << "\n";
    file << "projectSettingsCompactSidebar=" << (projectSettingsCompactSidebar ? "1" : "0") << "\n";
    file << "projectSettingsTwoColumns=" << (projectSettingsTwoColumns ? "1" : "0") << "\n";
    file << "projectSettingsVisibilityMode=" << std::clamp(projectSettingsVisibilityMode, 0, 2) << "\n";
    file << "showRegistryPackagesWindow=" << (showRegistryPackagesWindow ? "1" : "0") << "\n";
    file << "showGameProfilerWindow=" << (showGameProfilerWindow ? "1" : "0") << "\n";
    file << "hierarchyShowTexturePreview=" << (hierarchyShowTexturePreview ? "1" : "0") << "\n";
    file << "showSceneGizmos=" << (showSceneGizmos ? "1" : "0") << "\n";
    file << "gizmoShowCameraOverlays=" << (gizmoShowCameraOverlays ? "1" : "0") << "\n";
    file << "gizmoShowCameraFrustumLabels=" << (gizmoShowCameraFrustumLabels ? "1" : "0") << "\n";
    file << "gizmoShowLightOverlays=" << (gizmoShowLightOverlays ? "1" : "0") << "\n";
    file << "gizmoShowLightIntensityLabels=" << (gizmoShowLightIntensityLabels ? "1" : "0") << "\n";
    file << "gizmoShowInvariableLocks=" << (gizmoShowInvariableLocks ? "1" : "0") << "\n";
    file << "showViewportHintOverlay=" << (showViewportHintOverlay ? "1" : "0") << "\n";
    file << "showLight2DStatsOverlay=" << (showLight2DStatsOverlay ? "1" : "0") << "\n";
    file << "showLightmappingWindow=" << (showLightmappingWindow ? "1" : "0") << "\n";
    file << "blenderExecutablePath=" << blenderExecutableOverride << "\n";
    file << "showViewport=" << (showViewport ? "1" : "0") << "\n";
    file << "showGameViewport=" << (showGameViewport ? "1" : "0") << "\n";
    file << "showXRPreviewWindow=" << (showXRPreviewWindow ? "1" : "0") << "\n";
    file << "showVersionControlWindow=" << (showVersionControlWindow ? "1" : "0") << "\n";
    file << "xrPreviewPreset=" << xrPreview.presetIndex << "\n";
    file << "xrPreviewRenderScale=" << xrPreview.renderScale << "\n";
    file << "xrPreviewTargetHz=" << xrPreview.targetHz << "\n";
    file << "xrPreviewIpdMm=" << xrPreview.ipdMm << "\n";
    file << "xrPreviewFov=" << xrPreview.fovDegrees << "\n";
#if !MODULARITY_RUNTIME_ONLY
    file << "lightmapMode=" << static_cast<int>(lightmapping.settings.mode) << "\n";
    file << "lightmapQuality=" << static_cast<int>(lightmapping.settings.quality) << "\n";
    file << "lightmapBounces=" << lightmapping.settings.bounces << "\n";
    file << "lightmapFinalGatherRays=" << lightmapping.settings.finalGatherRays << "\n";
    file << "lightmapResolution=" << lightmapping.settings.resolution << "\n";
    file << "lightmapSkyIntensity=" << lightmapping.settings.skyLightIntensity << "\n";
    file << "lightmapInterpolation=" << lightmapping.settings.interpolation << "\n";
    file << "lightmapInterpolationPoints=" << lightmapping.settings.interpolationPoints << "\n";
    file << "lightmapLockAtlas=" << (lightmapping.settings.lockAtlas ? "1" : "0") << "\n";
    file << "lightmapLockedAtlasSize=" << lightmapping.settings.lockedAtlasSize << "\n";
#endif
    file << "sceneViewportRenderMode=" << static_cast<int>(sceneViewportRenderMode) << "\n";
    file << "maxRealtimeLights=" << renderer.getMaxRealtimeLights() << "\n";
    file << "world2DPostFxEnabled=" << (world2DPostFx.enabled ? "1" : "0") << "\n";
    file << "world2DPostFxDitherIntensity=" << std::clamp(world2DPostFx.ditherIntensity, 0.0f, 1.5f) << "\n";
    file << "world2DPostFxColorBits=" << std::clamp(world2DPostFx.colorBits, 1, 8) << "\n";
    file << "world2DPostFxDarkAdjustment=" << std::clamp(world2DPostFx.darkAdjustment, 0.0f, 1.0f) << "\n";
    file << "world2DPostFxDitherScale=" << std::clamp(world2DPostFx.ditherScale, 1.0f, 8.0f) << "\n";
    file << "world2DPostFxPixelation=" << std::clamp(world2DPostFx.pixelation, 0.0f, 64.0f) << "\n";
    file << "world2DPostFxExposure=" << std::clamp(world2DPostFx.exposure, -4.0f, 4.0f) << "\n";
    file << "world2DPostFxContrast=" << std::clamp(world2DPostFx.contrast, 0.0f, 2.5f) << "\n";
    file << "world2DPostFxSaturation=" << std::clamp(world2DPostFx.saturation, 0.0f, 2.5f) << "\n";
    file << "world2DPostFxColorFilterR=" << std::clamp(world2DPostFx.colorFilter.r, 0.0f, 2.0f) << "\n";
    file << "world2DPostFxColorFilterG=" << std::clamp(world2DPostFx.colorFilter.g, 0.0f, 2.0f) << "\n";
    file << "world2DPostFxColorFilterB=" << std::clamp(world2DPostFx.colorFilter.b, 0.0f, 2.0f) << "\n";
    file << "world2DPostFxVignetteIntensity=" << std::clamp(world2DPostFx.vignetteIntensity, 0.0f, 1.0f) << "\n";
    file << "world2DPostFxVignetteSmoothness=" << std::clamp(world2DPostFx.vignetteSmoothness, 0.05f, 1.0f) << "\n";
    file << "world2DPostFxChromaticAmount=" << std::clamp(world2DPostFx.chromaticAmount, 0.0f, 0.05f) << "\n";
    file << "world2DPostFxSharpenStrength=" << std::clamp(world2DPostFx.sharpenStrength, 0.0f, 2.0f) << "\n";
    file << "world2DPostFxGrainAmount=" << std::clamp(world2DPostFx.grainAmount, 0.0f, 0.4f) << "\n";
    file << "world2DPostFxScanlineIntensity=" << std::clamp(world2DPostFx.scanlineIntensity, 0.0f, 1.0f) << "\n";
    file << "gizmoShowLight2DBounds=" << (gizmoShowLight2DBounds ? "1" : "0") << "\n";
    file << "gizmoShowLight2DShapes=" << (gizmoShowLight2DShapes ? "1" : "0") << "\n";
    file << "gizmoShowShadowCaster2DBounds=" << (gizmoShowShadowCaster2DBounds ? "1" : "0") << "\n";
    file << "sceneGizmoIconScale=" << std::clamp(sceneGizmoIconScale, 0.4f, 3.0f) << "\n";
    file << "sceneGizmoOverlayScale=" << std::clamp(sceneGizmoOverlayScale, 0.4f, 3.0f) << "\n";
    file << "showSceneGrid3D=" << (showSceneGrid3D ? "1" : "0") << "\n";
    file << "uiCanvasPreviewEnabled=" << (uiCanvasPreviewEnabled ? "1" : "0") << "\n";
    file << "showCanvasOverlay=" << (showCanvasOverlay ? "1" : "0") << "\n";
    file << "showUIWorldGrid=" << (showUIWorldGrid ? "1" : "0") << "\n";
    file << "pixelGridSnapEnabled=" << (pixelGridSnapEnabled ? "1" : "0") << "\n";
    file << "pixelGridSnapStep=" << std::clamp(pixelGridSnapStep, 1, 64) << "\n";
    {
        const auto& tmPresentation = tmOpenGLRenderer.getPresentationSettings();
        file << "tmPresentationPitchStretchEnabled=" << (tmPresentation.lookPitchStretchEnabled ? "1" : "0") << "\n";
        file << "tmPresentationPitchStretchStrength=" << std::clamp(tmPresentation.lookPitchStretchStrength, 0.0f, 1.5f) << "\n";
        file << "tmPresentationPitchCompressStrength=" << std::clamp(tmPresentation.lookPitchCompressStrength, 0.0f, 1.5f) << "\n";
        file << "tmPresentationPitchShearStrength=" << std::clamp(tmPresentation.lookPitchShearStrength, 0.0f, 1.0f) << "\n";
        file << "tmPresentationPitchCurve=" << std::clamp(tmPresentation.lookPitchCurve, 0.1f, 4.0f) << "\n";
        file << "tmPresentationPitchDepthRange=" << std::clamp(tmPresentation.lookPitchDepthRange, 1.0f, 256.0f) << "\n";
        file << "tmPresentationPitchMinDegrees=" << std::clamp(tmPresentation.presentationPitchMinDegrees, -89.0f, 89.0f) << "\n";
        file << "tmPresentationPitchMaxDegrees=" << std::clamp(tmPresentation.presentationPitchMaxDegrees, -89.0f, 89.0f) << "\n";
        file << "tmPresentationWorldSnapEnabled=" << (tmPresentation.presentationSnapEnabled ? "1" : "0") << "\n";
        file << "tmPresentationWorldSnapStep=" << std::clamp(tmPresentation.presentationSnapStep, 0.001f, 8.0f) << "\n";
        file << "tmPresentationCameraSnapEnabled=" << (tmPresentation.cameraRelativeSnapEnabled ? "1" : "0") << "\n";
        file << "tmPresentationCameraSnapStep=" << std::clamp(tmPresentation.cameraRelativeSnapStep, 0.001f, 8.0f) << "\n";
        file << "tmPresentationVertexSnapEnabled=" << (tmPresentation.vertexSnapEnabled ? "1" : "0") << "\n";
        file << "tmPresentationVertexSnapStep=" << std::clamp(tmPresentation.vertexSnapStep, 0.0005f, 4.0f) << "\n";
        file << "tmPresentationScreenSnapEnabled=" << (tmPresentation.screenSnapEnabled ? "1" : "0") << "\n";
        file << "tmPresentationScreenSnapStep=" << std::clamp(tmPresentation.screenSnapStep, 0.25f, 16.0f) << "\n";
        file << "tmFake3DEnabled=" << (tmPresentation.fake3DEnabled ? "1" : "0") << "\n";
        file << "tmFake3DInternalHeight=" << std::clamp(tmPresentation.fake3DInternalHeight, 64, 2160) << "\n";
        file << "tmFake3DPointSampling=" << (tmPresentation.fake3DPointSampling ? "1" : "0") << "\n";
        file << "tmFake3DFlatShading=" << (tmPresentation.fake3DFlatShading ? "1" : "0") << "\n";
        file << "tmFake3DShadeLevels=" << std::clamp(tmPresentation.fake3DShadeLevels, 0, 16) << "\n";
        file << "tmFake3DAffineTextures=" << (tmPresentation.fake3DAffineTextures ? "1" : "0") << "\n";
        file << "tmFake3DAffineStrength=" << std::clamp(tmPresentation.fake3DAffineStrength, 0.0f, 1.0f) << "\n";
    }
    file << "showGameProfiler=" << (showGameProfiler ? "1" : "0") << "\n";
    file << "revealDebugSectionsAndMenus=" << (revealDebugSectionsAndMenus ? "1" : "0") << "\n";
    file << "collisionWireframe=" << (collisionWireframe ? "1" : "0") << "\n";
    file << "fpsCapEnabled=" << (fpsCapEnabled ? "1" : "0") << "\n";
    file << "fpsCap=" << fpsCap << "\n";
    file << "editorVSyncEnabled=" << (editorVSyncEnabled ? "1" : "0") << "\n";
    file << "cameraMoveSpeed=" << camera.moveSpeed << "\n";
    file << "cameraSprintSpeed=" << camera.sprintSpeed << "\n";
    file << "cameraSmoothMovement=" << (camera.smoothMovement ? "1" : "0") << "\n";
    file << "cameraAcceleration=" << camera.acceleration << "\n";
    file << "cameraMouseSensitivity=" << camera.mouseSensitivity << "\n";
    file << "gameViewportResolutionIndex=" << gameViewportResolutionIndex << "\n";
    file << "gameViewportCustomWidth=" << gameViewportCustomWidth << "\n";
    file << "gameViewportCustomHeight=" << gameViewportCustomHeight << "\n";
    file << "gameViewportAutoFit=" << (gameViewportAutoFit ? "1" : "0") << "\n";
    file << "gameViewportZoom=" << gameViewportZoom << "\n";
    file << "sceneViewportRenderWidth=" << sceneViewportRenderWidth << "\n";
    file << "sceneViewportRenderHeight=" << sceneViewportRenderHeight << "\n";
    auto writeViewportDisplayMode = [&file](const char* key, ViewportDisplayMode mode) {
        const char* modeName = "Stretch";
        switch (mode) {
            case ViewportDisplayMode::Fit: modeName = "Fit"; break;
            case ViewportDisplayMode::Fill: modeName = "Fill"; break;
            case ViewportDisplayMode::IntegerScale: modeName = "IntegerScale"; break;
            case ViewportDisplayMode::Stretch:
            default:
                modeName = "Stretch";
                break;
        }
        file << key << "=" << modeName << "\n";
    };
    writeViewportDisplayMode("sceneViewportDisplayMode", sceneViewportDisplayMode);
    writeViewportDisplayMode("gameViewportDisplayMode", gameViewportDisplayMode);
    const char* toolbarCornerName = "BottomLeft";
    switch (sceneViewportToolbarCorner) {
        case ViewportToolbarCorner::BottomRight: toolbarCornerName = "BottomRight"; break;
        case ViewportToolbarCorner::TopLeft: toolbarCornerName = "TopLeft"; break;
        case ViewportToolbarCorner::TopRight: toolbarCornerName = "TopRight"; break;
        case ViewportToolbarCorner::BottomLeft:
        default:
            toolbarCornerName = "BottomLeft";
            break;
    }
    file << "sceneViewportToolbarCorner=" << toolbarCornerName << "\n";
    file << "scriptAutoCompileInterval=" << scriptAutoCompileInterval << "\n";
    file << "scriptAutoCompileOnSave=" << (scriptEditorState.autoCompileOnSave ? "1" : "0") << "\n";
    file << "audioPreviewVolume=" << std::clamp(audioPreviewVolume, 0.0f, 2.0f) << "\n";
    file << "feedbackSoundsEnabled=" << (feedbackSoundsEnabled ? "1" : "0") << "\n";
    file << "feedbackClickSoundsEnabled=" << (feedbackClickSoundsEnabled ? "1" : "0") << "\n";
    file << "feedbackErrorSoundsEnabled=" << (feedbackErrorSoundsEnabled ? "1" : "0") << "\n";
    file << "feedbackOtherSoundsEnabled=" << (feedbackOtherSoundsEnabled ? "1" : "0") << "\n";
    file << "feedbackButtonSoundsEnabled=" << (feedbackButtonSoundsEnabled ? "1" : "0") << "\n";
    file << "feedbackToggleSoundsEnabled=" << (feedbackToggleSoundsEnabled ? "1" : "0") << "\n";
    file << "feedbackSlideSoundsEnabled=" << (feedbackSlideSoundsEnabled ? "1" : "0") << "\n";
    file << "feedbackTransitionSoundsEnabled=" << (feedbackTransitionSoundsEnabled ? "1" : "0") << "\n";
    file << "feedbackHoverSoundsEnabled=" << (feedbackHoverSoundsEnabled ? "1" : "0") << "\n";
    file << "feedbackSoundVolume=" << std::clamp(feedbackSoundVolume, 0.0f, 1.0f) << "\n";
    file << "stylePresetCount=" << uiStylePresets.size() << "\n";
    for (size_t i = 0; i < uiStylePresets.size(); ++i) {
        const UIStylePreset& preset = uiStylePresets[i];
        file << "stylePreset" << i << "_name=" << preset.name << "\n";
        file << "stylePreset" << i << "_builtin=" << (preset.builtin ? "1" : "0") << "\n";
        file << "stylePreset" << i << "_font=" << preset.fontAsset << "\n";
        file << "stylePreset" << i << "_styleBlob=" << EncodeStyleBlob(preset.style) << "\n";
        {
            std::ostringstream prefix;
            prefix << "stylePreset" << i << "_";
            WriteShadeTheme(file, prefix.str(), preset.shade);
        }
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4& c = style.Colors[i];
        file << "color." << ImGui::GetStyleColorName(i) << "="
             << c.x << "," << c.y << "," << c.z << "," << c.w << "\n";
    }

    fs::path baseRoot = fileBrowser.projectRoot.empty()
        ? projectManager.currentProject.projectPath
        : fileBrowser.projectRoot;
    for (const auto& fav : fileBrowserFavorites) {
        fs::path stored = fav;
        std::error_code ec;
        fs::path rel = fs::relative(fav, baseRoot, ec);
        std::string relStr = rel.generic_string();
        if (!ec && !rel.empty() && relStr.find("..") != 0) {
            stored = rel;
        }
        file << "favorite=" << stored.generic_string() << "\n";
    }
}

#pragma region Global Editor Preferences
void Engine::applyGlobalEditorPreferences(bool applyAppearance) {
    const EditorGlobalPreferences& p = projectManager.preferences;

    if (applyAppearance) {
        if (!p.themePreset.empty() && p.themePreset != uiStylePresetName) {
            applyUIStylePresetByName(p.themePreset);
        }
        if (!p.uiFontAsset.empty() && p.uiFontAsset != uiEditorFontAsset) {
            applyEditorUIFontById(p.uiFontAsset);
        }
    }

    if (p.chromeScale == "Compact") {
        uiChromeScale = EditorChromeScale::Compact;
    } else if (p.chromeScale == "Big") {
        uiChromeScale = EditorChromeScale::Big;
    } else {
        uiChromeScale = EditorChromeScale::Default;
    }

    if (p.animationMode == "Off") {
        uiAnimationMode = UIAnimationMode::Off;
    } else if (p.animationMode == "Snappy") {
        uiAnimationMode = UIAnimationMode::Snappy;
    } else {
        uiAnimationMode = UIAnimationMode::Fluid;
    }

    feedbackSoundsEnabled = p.feedbackSounds;
    feedbackClickSoundsEnabled = p.feedbackClickSounds;
    feedbackErrorSoundsEnabled = p.feedbackErrorSounds;
    feedbackOtherSoundsEnabled = p.feedbackOtherSounds;
    feedbackButtonSoundsEnabled = p.feedbackButtonSounds;
    feedbackToggleSoundsEnabled = p.feedbackToggleSounds;
    feedbackSlideSoundsEnabled = p.feedbackSlideSounds;
    feedbackTransitionSoundsEnabled = p.feedbackTransitionSounds;
    feedbackHoverSoundsEnabled = p.feedbackHoverSounds;
    feedbackSoundVolume = std::clamp(p.feedbackSoundVolume, 0.0f, 1.0f);
    editorSoundThemes.setActive(p.feedbackSoundTheme);

    hierarchyShowTexturePreview = p.hierarchyTexturePreviews;
    showSceneGizmos = p.sceneGizmos;
    gizmoShowCameraOverlays = p.gizmoCameraOverlays;
    consoleWrapText = p.consoleWrapText;
    quickToolsPinned = p.quickToolsPinned;

    editorVSyncEnabled = p.editorVSync;
    fpsCapEnabled = p.editorFpsCapEnabled;
    fpsCap = std::max(1.0f, p.editorFpsCap);
    scriptAutoCompileEnabled = p.scriptAutoCompile;
    scriptAutoCompileInterval = static_cast<double>(p.scriptAutoCompileInterval);
    scriptAutoCompileDirectoryScanInterval = static_cast<double>(p.scriptDirectoryScanInterval);

    syncGlassBlurToHardwareTier();
}

void Engine::syncGlassBlurToHardwareTier() {
    if (usingVulkan()) return;
    if (!ImGui::GetCurrentContext()) return;

    const bool wantGlass = projectManager.preferences.effectiveTier() !=
                           Modularity::HardwareProfile::Tier::Low;
    if (wantGlass) {
        Modularity::UiGlassBlur::Install();
    } else {
        Modularity::UiGlassBlur::Shutdown();
    }
}

void Engine::captureGlobalEditorPreferences() {
    EditorGlobalPreferences& p = projectManager.preferences;

    p.themePreset = uiStylePresetName;
    p.uiFontAsset = uiEditorFontAsset;
    switch (uiChromeScale) {
        case EditorChromeScale::Compact: p.chromeScale = "Compact"; break;
        case EditorChromeScale::Big:     p.chromeScale = "Big"; break;
        case EditorChromeScale::Default:
        default:                         p.chromeScale = "Default"; break;
    }
    switch (uiAnimationMode) {
        case UIAnimationMode::Off:    p.animationMode = "Off"; break;
        case UIAnimationMode::Snappy: p.animationMode = "Snappy"; break;
        case UIAnimationMode::Fluid:
        default:                      p.animationMode = "Fluid"; break;
    }

    p.feedbackSounds = feedbackSoundsEnabled;
    p.feedbackClickSounds = feedbackClickSoundsEnabled;
    p.feedbackErrorSounds = feedbackErrorSoundsEnabled;
    p.feedbackOtherSounds = feedbackOtherSoundsEnabled;
    p.feedbackButtonSounds = feedbackButtonSoundsEnabled;
    p.feedbackToggleSounds = feedbackToggleSoundsEnabled;
    p.feedbackSlideSounds = feedbackSlideSoundsEnabled;
    p.feedbackTransitionSounds = feedbackTransitionSoundsEnabled;
    p.feedbackHoverSounds = feedbackHoverSoundsEnabled;
    p.feedbackSoundVolume = feedbackSoundVolume;

    p.hierarchyTexturePreviews = hierarchyShowTexturePreview;
    p.sceneGizmos = showSceneGizmos;
    p.gizmoCameraOverlays = gizmoShowCameraOverlays;
    p.consoleWrapText = consoleWrapText;
    p.quickToolsPinned = quickToolsPinned;

    p.editorVSync = editorVSyncEnabled;
    p.editorFpsCapEnabled = fpsCapEnabled;
    p.editorFpsCap = fpsCap;
    p.scriptAutoCompile = scriptAutoCompileEnabled;
    p.scriptAutoCompileInterval = static_cast<float>(scriptAutoCompileInterval);
    p.scriptDirectoryScanInterval = static_cast<float>(scriptAutoCompileDirectoryScanInterval);
}

std::vector<std::string> Engine::installDefaultPackagesIntoProject() {
    std::vector<std::string> installed;
    const EditorGlobalPreferences& p = projectManager.preferences;
    if (!p.applyDefaultPackages || p.defaultPackageIds.empty()) {
        return installed;
    }

    for (const auto& id : p.defaultPackageIds) {
        if (id.empty() || packageManager.isInstalled(id)) {
            continue;
        }
        if (!packageManager.isCompatible(id)) {
            addConsoleMessage("Default ModuPAK skipped (incompatible): " + id,
                              ConsoleMessageType::Warning);
            continue;
        }
        if (packageManager.installRegistryPackageToProject(id)) {
            installed.push_back(id);
        } else {
            const std::string& err = packageManager.getLastError();
            addConsoleMessage("Default ModuPAK failed: " + id +
                                  (err.empty() ? std::string() : (" - " + err)),
                              ConsoleMessageType::Warning);
        }
    }
    return installed;
}
#pragma endregion

#pragma region Theme Hot Reload
fs::path Engine::getEditorThemeFilePath() const {
    if (!projectManager.currentProject.isLoaded) {
        return fs::path();
    }
    return projectManager.currentProject.projectPath / "ProjectUserSettings" / "EditorTheme.modutheme";
}

bool Engine::loadEditorThemeFile(const fs::path& path, std::string* outError) {
    std::ifstream file(path);
    if (!file.is_open()) {
        if (outError) *outError = "could not open " + path.string();
        return false;
    }

    static std::unordered_map<std::string, int> colorIndex;
    if (colorIndex.empty()) {
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            colorIndex.emplace(ImGui::GetStyleColorName(i), i);
        }
    }

    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiShadeTheme shade = ImGui::GetShadeTheme();
    bool touchedShade = false;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = RenameTrim(line.substr(0, eq));
        const std::string value = RenameTrim(line.substr(eq + 1));

        if (key.rfind("color.", 0) == 0) {
            auto it = colorIndex.find(key.substr(6));
            if (it == colorIndex.end()) continue;
            std::string parseValue = value;
            std::replace(parseValue.begin(), parseValue.end(), ',', ' ');
            std::istringstream ss(parseValue);
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
            if (ss >> r >> g >> b) {
                if (!(ss >> a)) a = 1.0f;
                style.Colors[it->second] = ImVec4(r, g, b, a);
            }
        } else if (ParseShadeThemeField(key, value, shade, nullptr)) {
            touchedShade = true;
        }
    }

    if (touchedShade) {
        shade.Scale = (uiDpiScale > 0.0f) ? uiDpiScale : 1.0f;
        ImGui::SetShadeTheme(shade);
    }
    return true;
}

bool Engine::saveEditorThemeFile(std::string* outError) {
    const fs::path themePath = getEditorThemeFilePath();
    if (themePath.empty()) {
        if (outError) *outError = "no project loaded";
        return false;
    }
    std::error_code ec;
    fs::create_directories(themePath.parent_path(), ec);
    std::ofstream file(themePath);
    if (!file.is_open()) {
        if (outError) *outError = "could not write " + themePath.string();
        return false;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    file << "# Modularity editor theme. Edited while the editor is running and it reloads within a second.\n";
    file << "# Written from preset: " << uiStylePresetName << "\n\n";
    file << std::fixed << std::setprecision(4);
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4& c = style.Colors[i];
        file << "color." << ImGui::GetStyleColorName(i) << "="
             << c.x << "," << c.y << "," << c.z << "," << c.w << "\n";
    }
    file << "\n";
    WriteShadeTheme(file, std::string(), ImGui::GetShadeTheme());
    file.close();

    const fs::file_time_type writeTime = fs::last_write_time(themePath, ec);
    if (!ec) {
        editorThemeFileLastWrite = writeTime;
        editorThemeFileTracked = true;
    }
    return true;
}

void Engine::pollEditorThemeFile() {
    const double now = glfwGetTime();
    if (now < editorThemeFileNextCheck) {
        return;
    }
    editorThemeFileNextCheck = now + 1.0;

    const fs::path themePath = getEditorThemeFilePath();
    if (themePath.empty()) {
        return;
    }
    std::error_code ec;
    if (!fs::exists(themePath, ec) || ec) {
        editorThemeFileTracked = false;
        return;
    }
    const fs::file_time_type writeTime = fs::last_write_time(themePath, ec);
    if (ec) {
        return;
    }
    if (editorThemeFileTracked && writeTime == editorThemeFileLastWrite) {
        return;
    }
    const bool firstSight = !editorThemeFileTracked;
    editorThemeFileTracked = true;
    editorThemeFileLastWrite = writeTime;

    std::string error;
    if (loadEditorThemeFile(themePath, &error)) {
        addConsoleMessage(firstSight ? "Loaded editor theme: " + themePath.filename().string()
                                     : "Reloaded editor theme: " + themePath.filename().string(),
                          ConsoleMessageType::Info);
    } else {
        addConsoleMessage("Editor theme reload failed: " + error, ConsoleMessageType::Warning);
    }
}
#pragma endregion

void Engine::exportEditorThemeLayout() {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("No project loaded to export UI settings", ConsoleMessageType::Warning);
        return;
    }
    saveEditorUserSettings();
    fs::path layoutPath = getWorkspaceLayoutPath(currentWorkspace);
    if (layoutPath.empty()) {
        addConsoleMessage("Failed to resolve layout export path", ConsoleMessageType::Error);
        return;
    }
    fs::create_directories(layoutPath.parent_path());
    ImGui::SaveIniSettingsToDisk(layoutPath.string().c_str());
    addConsoleMessage("Exported UI layout to: " + layoutPath.string(), ConsoleMessageType::Success);
}
