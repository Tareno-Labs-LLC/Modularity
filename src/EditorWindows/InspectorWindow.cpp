#include "../ScriptContextBinding.h"
#include "UIInspector.h"
#include "../EditorLocalization.h"
#include "Engine.h"
#include "HardwareProfile.h"
#include "MapMaker.h"
#include "MaterialAssetUtils.h"
#include "ModelLoader.h"
#include "ParticleSystemRuntime.h"
#include "SceneObjectIcons.h"
#include "../../SpritesheetFormat.h"
#include "DragPreviewOverlay.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <functional>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <regex>
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <future>
#include <chrono>
#include <future>
#include <type_traits>

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace Loc = Modularity::Loc;

static std::string InspectorLocKey(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isalnum(c) != 0) {
            const unsigned char prev = i > 0 ? static_cast<unsigned char>(text[i - 1]) : 0;
            if (std::isupper(c) != 0 && (std::islower(prev) != 0 || std::isdigit(prev) != 0) &&
                !out.empty() && out.back() != '_') {
                out.push_back('_');
            }
            out.push_back(static_cast<char>(std::toupper(c)));
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

namespace ImGui {
    bool BufferingBar(const char* label, float value, const ImVec2& size_arg, const ImU32& bg_col, const ImU32& fg_col);
}

#pragma region Inspector Helpers
namespace {
    bool IsNativeBinaryPath(const fs::path& path) {
        const std::string ext = path.extension().string();
        return ext == ".so" || ext == ".dll" || ext == ".dylib";
    }

    std::string LowerExtension(const fs::path& path) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    // Sources a native Script component compiles: ModuCPP (and the ModuMako front end that
    // lowers to it), C++ and C. The Script slot's filter and asset drops both use this.
    // (4eb6f8ef deleted the lambdas these replace but kept their call sites in the Script slot.)
    bool isNativeScriptSourcePath(const fs::path& path) {
        const std::string ext = LowerExtension(path);
        return ext == ".moducpp" || ext == ".mko" || ext == ".modumako" ||
               ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c";
    }

    ScriptLanguage inferNativeLanguageFromPath(const fs::path& path) {
        return LowerExtension(path) == ".c" ? ScriptLanguage::C : ScriptLanguage::Cpp;
    }

    bool IsSpriteSheetSidecarPath(const fs::path& path) {
        return path.extension() == ".spritesheet";
    }

    fs::path ResolveSpriteSheetImagePath(const fs::path& path) {
        if (!IsSpriteSheetSidecarPath(path)) {
            return path;
        }
        fs::path imagePath = path;
        imagePath.replace_extension();
        return imagePath;
    }

    std::optional<SpritesheetDocument> LoadSpriteSheetDocument(const fs::path& sidecarPath) {
        std::ifstream sidecar(sidecarPath);
        if (!sidecar.is_open()) {
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << sidecar.rdbuf();
        return ParseSpritesheet(buffer.str()).document;
    }

    std::optional<std::string> InferManagedTypeFromSource(const std::string& source,
                                                          const std::string& fallbackClass) {
        std::string nameSpace;
        std::string className;
        try {
            std::smatch match;
            std::regex namespacePattern(R"(namespace\s+([A-Za-z_][A-Za-z0-9_\.]*)\s*(\{|;))");
            if (std::regex_search(source, match, namespacePattern) && match.size() > 1) {
                nameSpace = match[1].str();
            }

            if (!fallbackClass.empty()) {
                auto escapeRegex = [](const std::string& value) {
                    std::string escaped;
                    escaped.reserve(value.size() * 2);
                    for (char c : value) {
                        if (c == '\\' || c == '.' || c == '+' || c == '*' || c == '?' || c == '^' || c == '$' ||
                            c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|') {
                            escaped.push_back('\\');
                        }
                        escaped.push_back(c);
                    }
                    return escaped;
                };
                std::regex classMatch("\\bclass\\s+" + escapeRegex(fallbackClass) + "\\b");
                if (std::regex_search(source, classMatch)) {
                    className = fallbackClass;
                }
            }

            if (className.empty()) {
                std::regex classPattern(R"(\bclass\s+([A-Za-z_][A-Za-z0-9_]*))");
                if (std::regex_search(source, match, classPattern) && match.size() > 1) {
                    className = match[1].str();
                }
            }
        } catch (...) {
            return std::nullopt;
        }

        if (className.empty()) return std::nullopt;
        if (!nameSpace.empty()) return nameSpace + "." + className;
        return className;
    }

    std::optional<std::string> InferManagedTypeFromFile(const fs::path& path) {
        std::ifstream file(path);
        if (!file.is_open()) return std::nullopt;
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string source = ss.str();
        std::string fallback = path.stem().string();
        return InferManagedTypeFromSource(source, fallback);
    }

    void UpdateLegacyTypeFromComponents(SceneObject& target) {
        if (target.type == ObjectType::Sprite25D) {
            return;
        }
        if (target.hasParticleSystem2D || target.type == ObjectType::ParticleSystem2D) {
            target.type = ObjectType::ParticleSystem2D;
            return;
        }
        if (target.hasRenderer) {
            switch (target.renderType) {
                case RenderType::Cube: target.type = ObjectType::Cube; break;
                case RenderType::Sphere: target.type = ObjectType::Sphere; break;
                case RenderType::Capsule: target.type = ObjectType::Capsule; break;
                case RenderType::OBJMesh: target.type = ObjectType::OBJMesh; break;
                case RenderType::Model: target.type = ObjectType::Model; break;
                case RenderType::Mirror: target.type = ObjectType::Mirror; break;
                case RenderType::Plane: target.type = ObjectType::Plane; break;
                case RenderType::Torus: target.type = ObjectType::Torus; break;
                case RenderType::Sprite: target.type = ObjectType::Sprite; break;
                case RenderType::None: break;
            }
            return;
        }
        if (target.hasUI) {
            switch (target.ui.type) {
                case UIElementType::Canvas: target.type = ObjectType::Canvas; break;
                case UIElementType::Image: target.type = ObjectType::UIImage; break;
                case UIElementType::Slider: target.type = ObjectType::UISlider; break;
                case UIElementType::Button: target.type = ObjectType::UIButton; break;
                case UIElementType::Text: target.type = ObjectType::UIText; break;
                case UIElementType::Sprite2D: target.type = ObjectType::Sprite2D; break;
                case UIElementType::None: break;
                default: target.type = ObjectType::UIImage; break;
            }
            return;
        }
        if (target.hasLight) {
            switch (target.light.type) {
                case LightType::Directional: target.type = ObjectType::DirectionalLight; break;
                case LightType::Point: target.type = ObjectType::PointLight; break;
                case LightType::Spot: target.type = ObjectType::SpotLight; break;
                case LightType::Area: target.type = ObjectType::AreaLight; break;
            }
            return;
        }
        if (target.hasReflectionCast) {
            target.type = ObjectType::ReflectionCast;
            return;
        }
        if (target.hasLight2D) {
            switch (target.light2D.type) {
                case Light2DType::Point: target.type = ObjectType::Light2D; break;
                case Light2DType::Spot: target.type = ObjectType::Light2D; break;
                case Light2DType::Freeform: target.type = ObjectType::Light2D; break;
                case Light2DType::Sprite: target.type = ObjectType::Light2D; break;
                case Light2DType::Global: target.type = ObjectType::Light2D; break;
            }
            return;
        }
        if (target.hasShadowCaster2D) {
            target.type = ObjectType::ShadowCaster2D;
            return;
        }
        if (target.hasCamera) {
            target.type = ObjectType::Camera;
            return;
        }
        if (target.hasPostFX) {
            target.type = ObjectType::PostFXNode;
            return;
        }
        target.type = ObjectType::Empty;
    }

    bool MoveInspectorComponentBefore(SceneObject& obj,
                                      const std::string& movingKey,
                                      const std::string& targetKey) {
        EnsureInspectorComponentMetadata(obj);
        if (movingKey.empty() || targetKey.empty() || movingKey == targetKey) {
            return false;
        }

        auto movingIt = std::find(obj.inspectorComponentOrder.begin(), obj.inspectorComponentOrder.end(), movingKey);
        auto targetIt = std::find(obj.inspectorComponentOrder.begin(), obj.inspectorComponentOrder.end(), targetKey);
        if (movingIt == obj.inspectorComponentOrder.end() || targetIt == obj.inspectorComponentOrder.end()) {
            return false;
        }

        const std::string key = *movingIt;
        const ptrdiff_t targetIndex = std::distance(obj.inspectorComponentOrder.begin(), targetIt);
        obj.inspectorComponentOrder.erase(movingIt);
        auto insertIt = obj.inspectorComponentOrder.begin() +
            std::clamp<ptrdiff_t>(targetIndex, 0, static_cast<ptrdiff_t>(obj.inspectorComponentOrder.size()));
        obj.inspectorComponentOrder.insert(insertIt, key);
        return true;
    }

    bool MoveInspectorComponentByOffset(SceneObject& obj,
                                        const std::string& key,
                                        int offset) {
        EnsureInspectorComponentMetadata(obj);
        auto it = std::find(obj.inspectorComponentOrder.begin(), obj.inspectorComponentOrder.end(), key);
        if (it == obj.inspectorComponentOrder.end() || offset == 0) {
            return false;
        }

        const ptrdiff_t index = std::distance(obj.inspectorComponentOrder.begin(), it);
        const ptrdiff_t targetIndex = std::clamp<ptrdiff_t>(
            index + offset,
            0,
            static_cast<ptrdiff_t>(obj.inspectorComponentOrder.size()) - 1);
        if (targetIndex == index) {
            return false;
        }

        const std::string movedKey = *it;
        obj.inspectorComponentOrder.erase(it);
        obj.inspectorComponentOrder.insert(obj.inspectorComponentOrder.begin() + targetIndex, movedKey);
        return true;
    }

    bool MoveInspectorComponentToEdge(SceneObject& obj,
                                      const std::string& key,
                                      bool toTop) {
        EnsureInspectorComponentMetadata(obj);
        auto it = std::find(obj.inspectorComponentOrder.begin(), obj.inspectorComponentOrder.end(), key);
        if (it == obj.inspectorComponentOrder.end()) {
            return false;
        }

        const ptrdiff_t index = std::distance(obj.inspectorComponentOrder.begin(), it);
        if ((toTop && index == 0) ||
            (!toTop && index == static_cast<ptrdiff_t>(obj.inspectorComponentOrder.size()) - 1)) {
            return false;
        }

        const std::string movedKey = *it;
        obj.inspectorComponentOrder.erase(it);
        if (toTop) {
            obj.inspectorComponentOrder.insert(obj.inspectorComponentOrder.begin(), movedKey);
        } else {
            obj.inspectorComponentOrder.push_back(movedKey);
        }
        return true;
    }

    void ApplyReverbPreset(ReverbZoneComponent& zone, ReverbPreset preset) {
        zone.preset = preset;
        switch (preset) {
            case ReverbPreset::Room:
                zone.room = -1000.0f;
                zone.roomHF = -500.0f;
                zone.roomLF = 0.0f;
                zone.decayTime = 1.2f;
                zone.decayHFRatio = 0.8f;
                zone.reflections = -2600.0f;
                zone.reflectionsDelay = 0.01f;
                zone.reverb = 100.0f;
                zone.reverbDelay = 0.012f;
                zone.hfReference = 5000.0f;
                zone.lfReference = 250.0f;
                zone.roomRolloffFactor = 0.0f;
                zone.diffusion = 85.0f;
                zone.density = 90.0f;
                break;
            case ReverbPreset::LivingRoom:
                zone.room = -1200.0f;
                zone.roomHF = -800.0f;
                zone.roomLF = 0.0f;
                zone.decayTime = 1.5f;
                zone.decayHFRatio = 0.7f;
                zone.reflections = -2400.0f;
                zone.reflectionsDelay = 0.02f;
                zone.reverb = 150.0f;
                zone.reverbDelay = 0.015f;
                zone.hfReference = 5000.0f;
                zone.lfReference = 250.0f;
                zone.roomRolloffFactor = 0.0f;
                zone.diffusion = 90.0f;
                zone.density = 95.0f;
                break;
            case ReverbPreset::Hall:
                zone.room = -1000.0f;
                zone.roomHF = -200.0f;
                zone.roomLF = 0.0f;
                zone.decayTime = 3.2f;
                zone.decayHFRatio = 0.7f;
                zone.reflections = -1500.0f;
                zone.reflectionsDelay = 0.03f;
                zone.reverb = 500.0f;
                zone.reverbDelay = 0.02f;
                zone.hfReference = 5000.0f;
                zone.lfReference = 250.0f;
                zone.roomRolloffFactor = 0.0f;
                zone.diffusion = 95.0f;
                zone.density = 100.0f;
                break;
            case ReverbPreset::Forest:
                zone.room = -1500.0f;
                zone.roomHF = -1800.0f;
                zone.roomLF = 0.0f;
                zone.decayTime = 1.1f;
                zone.decayHFRatio = 0.3f;
                zone.reflections = -3000.0f;
                zone.reflectionsDelay = 0.02f;
                zone.reverb = -100.0f;
                zone.reverbDelay = 0.01f;
                zone.hfReference = 2500.0f;
                zone.lfReference = 150.0f;
                zone.roomRolloffFactor = 0.0f;
                zone.diffusion = 50.0f;
                zone.density = 60.0f;
                break;
            case ReverbPreset::Custom:
            default:
                break;
        }
    }
}

void EnsureSpriteClipNames(std::vector<std::string>& names, size_t count) {
    if (names.size() < count) {
        for (size_t i = names.size(); i < count; ++i) {
            names.push_back("Rect_" + std::to_string(i));
        }
    } else if (names.size() > count) {
        names.resize(count);
    }
}

void EnsureSpriteClipScales(std::vector<glm::vec2>& scales, size_t count) {
    if (scales.size() < count) {
        scales.resize(count, glm::vec2(1.0f));
    } else if (scales.size() > count) {
        scales.resize(count);
    }
    for (glm::vec2& scale : scales) {
        scale.x = std::max(0.01f, scale.x);
        scale.y = std::max(0.01f, scale.y);
    }
}

#pragma region Reference Slots
// One drag-and-drop assigner shared by every reference field in the editor. The row carries an
// icon on the left - a live thumbnail for images, the same component icon the Hierarchy uses
// for SceneOBJs, the Project's file-type icon for everything else - and the reference's name
// beside it, or "None (<type>)" when it is empty so the field says what it wants dropped on it.
//
// This replaces the old "Use Selection" / "Clear" button pairs: dropping assigns, left-click
// opens a searchable picker, and right-click holds Clear, Use Project Selection and Reveal.
namespace {

constexpr float kReferenceSlotLabelWidth = 126.0f;

std::string FitLabelToWidth(const std::string& text, float maxWidth) {
    const float safeWidth = std::max(12.0f, maxWidth);
    if (ImGui::CalcTextSize(text.c_str()).x <= safeWidth) {
        return text;
    }
    constexpr const char* kEllipsis = "...";
    std::string clipped = text;
    while (!clipped.empty()) {
        std::string candidate = clipped + kEllipsis;
        if (ImGui::CalcTextSize(candidate.c_str()).x <= safeWidth) {
            return candidate;
        }
        clipped.pop_back();
    }
    return std::string(kEllipsis);
}

// Inline label to the left of a value control, clipped rather than wrapped so a long field
// name can never push the control off the panel.
void DrawInspectorInlineLabel(const char* label, float width) {
    ImGui::AlignTextToFramePadding();

    const float rowHeight = std::max(20.0f, ImGui::GetFrameHeight());
    const ImVec2 labelMin = ImGui::GetCursorScreenPos();

    ImGui::Dummy(ImVec2(width, rowHeight));

    const std::string clippedLabel = FitLabelToWidth(label, std::max(12.0f, width - 6.0f));
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->PushClipRect(labelMin, ImVec2(labelMin.x + width, labelMin.y + rowHeight), true);
    drawList->AddText(
        ImVec2(labelMin.x, labelMin.y + std::max(0.0f, (rowHeight - ImGui::GetTextLineHeight()) * 0.5f)),
        ImGui::GetColorU32(ImGuiCol_TextDisabled),
        clippedLabel.c_str());
    drawList->PopClipRect();

    if (clippedLabel != label && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", label);
    }
}

// Checkerboard behind a thumbnail so a texture's alpha reads as transparency rather than as
// whatever colour happens to sit under the panel.
void DrawTransparencyCheckerboard(ImDrawList* drawList,
                                  const ImVec2& minPos,
                                  const ImVec2& maxPos,
                                  float rounding,
                                  float cellSize = 10.0f) {
    drawList->AddRectFilled(minPos, maxPos, IM_COL32(34, 37, 44, 120), rounding);

    const ImU32 lightCell = IM_COL32(78, 84, 96, 90);
    const ImU32 darkCell = IM_COL32(52, 57, 68, 90);
    const int cols = std::max(1, static_cast<int>(std::ceil((maxPos.x - minPos.x) / cellSize)));
    const int rows = std::max(1, static_cast<int>(std::ceil((maxPos.y - minPos.y) / cellSize)));
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            const float x0 = minPos.x + x * cellSize;
            const float y0 = minPos.y + y * cellSize;
            const float x1 = std::min(maxPos.x, x0 + cellSize);
            const float y1 = std::min(maxPos.y, y0 + cellSize);
            drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                                    ((x + y) & 1) == 0 ? lightCell : darkCell, 0.0f);
        }
    }

    drawList->AddRect(minPos, maxPos, IM_COL32(92, 100, 116, 120), rounding, 0, 1.0f);
}

bool MatchesAssetFilter(const std::string& text, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    return MaterialAssetLowercase(text).find(MaterialAssetLowercase(filter)) != std::string::npos;
}

std::vector<fs::path> CollectAssetsInDirectory(
        const fs::path& root,
        const std::function<bool(const fs::directory_entry&)>& predicate) {
    std::vector<fs::path> matches;
    if (root.empty()) {
        return matches;
    }

    std::error_code ec;
    if (!fs::exists(root, ec) || ec) {
        return matches;
    }

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const fs::directory_entry& entry = *it;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        if (predicate(entry)) {
            matches.push_back(entry.path());
        }
    }

    std::sort(matches.begin(), matches.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return matches;
}

// The frame every reference slot draws itself into. Recessed like the editor's other input
// frames, so a slot reads as something you drop into rather than as a button.
struct ReferenceSlotFrame {
    ImVec2 min{};
    ImVec2 max{};
    ImVec2 iconMin{};
    ImVec2 iconMax{};
    float textLeft = 0.0f;
    float rowHeight = 0.0f;
    bool pressed = false;
    bool hovered = false;
};

ReferenceSlotFrame BeginReferenceSlotFrame(const char* strId, bool dropCandidate) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ReferenceSlotFrame frame;
    frame.rowHeight = std::max(20.0f, ImGui::GetFrameHeight());
    const float width = std::max(48.0f, ImGui::GetContentRegionAvail().x);

    frame.pressed = ImGui::InvisibleButton(strId, ImVec2(width, frame.rowHeight));
    frame.hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    frame.min = ImGui::GetItemRectMin();
    frame.max = ImGui::GetItemRectMax();

    const ImU32 fill = ImGui::GetColorU32(held ? ImGuiCol_FrameBgActive
                                               : (frame.hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImGui::ShadeRect(drawList, frame.min, frame.max, fill, ImGuiShadeClass_Frame,
                     ImGui::ShadeStateFromInteraction(frame.hovered, held), style.FrameRounding);
    if (dropCandidate) {
        drawList->AddRect(frame.min, frame.max, ImGui::GetColorU32(ImGuiCol_DragDropTarget),
                          style.FrameRounding, 0, 2.0f);
    }

    const float inset = 2.0f;
    const float iconSize = std::max(10.0f, frame.rowHeight - inset * 2.0f);
    frame.iconMin = ImVec2(frame.min.x + inset, frame.min.y + (frame.rowHeight - iconSize) * 0.5f);
    frame.iconMax = ImVec2(frame.iconMin.x + iconSize, frame.iconMin.y + iconSize);
    frame.textLeft = frame.iconMax.x + 6.0f;
    return frame;
}

void DrawReferenceSlotText(const ReferenceSlotFrame& frame, const std::string& text, bool empty) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float avail = std::max(8.0f, frame.max.x - frame.textLeft - 6.0f);
    const std::string clipped = FitLabelToWidth(text, avail);
    drawList->PushClipRect(ImVec2(frame.textLeft, frame.min.y), ImVec2(frame.max.x - 4.0f, frame.max.y), true);
    drawList->AddText(
        ImVec2(frame.textLeft, frame.min.y + std::max(0.0f, (frame.rowHeight - ImGui::GetTextLineHeight()) * 0.5f)),
        ImGui::GetColorU32(empty ? ImGuiCol_TextDisabled : ImGuiCol_Text),
        clipped.c_str());
    drawList->PopClipRect();
}

// Drawn in place of an icon that could not be loaded, so the slot never shows a blank gap
// where the type marker belongs.
void DrawReferenceSlotIconPlaceholder(const ReferenceSlotFrame& frame, bool empty) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled, empty ? 0.45f : 0.85f);
    drawList->AddRect(frame.iconMin, frame.iconMax, color, 3.0f, 0, 1.0f);
}

}  // namespace

bool Engine::drawAssetReferenceSlot(const char* id,
                                    const char* label,
                                    const std::string& currentPath,
                                    FileCategory expectedCategory,
                                    const char* typeName,
                                    fs::path& outAssigned,
                                    const ReferenceSlotFilter& customFilter) {
    outAssigned.clear();
    bool changed = false;

    ImGui::PushID(id);

    const bool wantsTexture = (expectedCategory == FileCategory::Texture);
    const std::string typeLabel = (typeName && *typeName) ? std::string(typeName) : std::string("Asset");
    const std::string emptyText = "None (" + typeLabel + ")";
    const fs::path resolved = resolveProjectAssetPath(currentPath);

    // FileCategory::Unknown means "any regular file". Every other category is matched exactly,
    // so a Sound Clip slot cannot swallow a mesh.
    auto accepts = [&](const fs::path& candidate) -> bool {
        if (candidate.empty()) {
            return false;
        }
        std::error_code ec;
        if (wantsTexture && IsSpriteSheetSidecarPath(candidate)) {
            return fs::exists(ResolveSpriteSheetImagePath(candidate), ec) && !ec;
        }
        fs::directory_entry entry(candidate, ec);
        if (ec) {
            return false;
        }
        if (!entry.is_regular_file(ec) || ec) {
            return false;
        }
        if (customFilter) {
            return customFilter(candidate);
        }
        if (expectedCategory == FileCategory::Unknown) {
            return true;
        }
        return fileBrowser.getFileCategory(entry) == expectedCategory;
    };
    auto assign = [&](const fs::path& candidate) {
        outAssigned = wantsTexture ? ResolveSpriteSheetImagePath(candidate) : candidate;
        changed = true;
    };

    if (label && *label) {
        DrawInspectorInlineLabel(label, kReferenceSlotLabelWidth);
        ImGui::SameLine();
    }

    bool dropCandidate = false;
    if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload()) {
        if (dragging->IsDataType("FILE_PATH") && dragging->Data) {
            dropCandidate = accepts(fs::path(static_cast<const char*>(dragging->Data)));
        }
    }

    const ReferenceSlotFrame frame = BeginReferenceSlotFrame("##Slot", dropCandidate);
    const char* popupId = "##ReferenceSlotPicker";
    if (frame.pressed) {
        ImGui::OpenPopup(popupId);
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
            const char* dropped = static_cast<const char*>(payload->Data);
            if (dropped && *dropped) {
                const fs::path droppedPath(dropped);
                if (accepts(droppedPath)) {
                    assign(droppedPath);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Icon: a live thumbnail for an assigned image, otherwise the Project's file-type icon -
    // dimmed while the slot is empty, so an empty row still shows what it takes.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    bool iconDrawn = false;
    if (rendererInitialized) {
        if (wantsTexture && !currentPath.empty()) {
            Texture* thumb = renderer.getTexture(resolved.string(),
                                                 MaterialProperties::TextureFilter::Bilinear);
            if (thumb && thumb->GetID()) {
                DrawTransparencyCheckerboard(drawList, frame.iconMin, frame.iconMax, 3.0f, 6.0f);
                drawList->AddImage((ImTextureID)(intptr_t)thumb->GetID(),
                                   frame.iconMin, frame.iconMax, ImVec2(0, 1), ImVec2(1, 0));
                iconDrawn = true;
            }
        }
        if (!iconDrawn) {
            Texture* icon = renderer.getTexture(AssetCategoryIconPath(expectedCategory),
                                                MaterialProperties::TextureFilter::Bilinear);
            if (icon && icon->GetID()) {
                drawList->AddImage((ImTextureID)(intptr_t)icon->GetID(),
                                   frame.iconMin, frame.iconMax, ImVec2(0, 1), ImVec2(1, 0),
                                   currentPath.empty() ? IM_COL32(255, 255, 255, 96) : IM_COL32_WHITE);
                iconDrawn = true;
            }
        }
    }
    if (!iconDrawn) {
        DrawReferenceSlotIconPlaceholder(frame, currentPath.empty());
    }

    std::string displayName = emptyText;
    if (!currentPath.empty()) {
        displayName = resolved.stem().string();
        if (displayName.empty()) {
            displayName = resolved.filename().string();
        }
        if (displayName.empty()) {
            displayName = currentPath;
        }
    }
    DrawReferenceSlotText(frame, displayName, currentPath.empty());

    if (frame.hovered) {
        if (currentPath.empty()) {
            ImGui::SetTooltip("Drag a %s here, or click to pick one.", typeLabel.c_str());
        } else {
            ImGui::SetTooltip("%s\nClick to pick another, right-click for more.", currentPath.c_str());
        }
    }

    const bool selectionMatches = accepts(fileBrowser.selectedFile);
    if (ImGui::BeginPopupContextItem("##ReferenceSlotMenu")) {
        ImGui::BeginDisabled(currentPath.empty());
        if (ImGui::MenuItem("Clear")) {
            outAssigned.clear();
            changed = true;
        }
        if (ImGui::MenuItem("Reveal in Project")) {
            std::error_code ec;
            if (fs::exists(resolved, ec) && !ec) {
                fileBrowser.navigateTo(resolved.parent_path());
                fileBrowser.selectedFile = resolved;
            }
        }
        if (ImGui::MenuItem("Copy Path")) {
            ImGui::SetClipboardText(currentPath.c_str());
        }
        ImGui::EndDisabled();
        // The slot is not a text field, so pasting is how a path from outside the Project
        // tree still gets in.
        const char* clipboard = ImGui::GetClipboardText();
        const bool clipboardMatches = clipboard && *clipboard && accepts(fs::path(clipboard));
        ImGui::BeginDisabled(!clipboardMatches);
        if (ImGui::MenuItem("Paste Path")) {
            assign(fs::path(clipboard));
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(!selectionMatches);
        if (ImGui::MenuItem("Use Project Selection")) {
            assign(fileBrowser.selectedFile);
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup(popupId)) {
        static std::unordered_map<std::string, std::string> pickerFilters;
        std::string& filter = pickerFilters[std::string(id)];
        char searchBuf[128] = {};
        std::snprintf(searchBuf, sizeof(searchBuf), "%s", filter.c_str());
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::InputTextWithHint("##Filter", ("Filter " + typeLabel).c_str(),
                                     searchBuf, sizeof(searchBuf))) {
            filter = searchBuf;
        }

        if (ImGui::Selectable(emptyText.c_str(), currentPath.empty())) {
            outAssigned.clear();
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        if (selectionMatches && ImGui::Selectable("Use Project Selection")) {
            assign(fileBrowser.selectedFile);
            ImGui::CloseCurrentPopup();
        }

        ImGui::Separator();
        ImGui::BeginChild("##ReferenceSlotList", ImVec2(320.0f, 180.0f), false);
        const fs::path assetRoot = projectManager.currentProject.isLoaded
            ? projectManager.currentProject.assetsPath
            : fileBrowser.projectRoot;
        const std::vector<fs::path> candidates = CollectAssetsInDirectory(
            assetRoot, [&](const fs::directory_entry& entry) { return accepts(entry.path()); });
        for (const fs::path& assetPath : candidates) {
            const std::string assetName = assetPath.filename().string();
            if (!MatchesAssetFilter(assetName, filter)) {
                continue;
            }
            const bool selected = (resolved == assetPath);
            if (ImGui::Selectable((assetName + "##" + assetPath.string()).c_str(), selected)) {
                assign(assetPath);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", assetPath.string().c_str());
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    ImGui::PopID();
    return changed;
}

bool Engine::drawAssetReferenceSlot(const char* id,
                                    const char* label,
                                    std::string& path,
                                    FileCategory expectedCategory,
                                    const char* typeName) {
    fs::path assigned;
    if (!drawAssetReferenceSlot(id, label, path, expectedCategory, typeName, assigned)) {
        return false;
    }
    const std::string next = assigned.empty() ? std::string() : assigned.string();
    if (next == path) {
        return false;
    }
    path = next;
    return true;
}

bool Engine::drawObjectReferenceSlot(const char* id,
                                     const char* label,
                                     int& targetId,
                                     int disallowId,
                                     const char* typeName) {
    bool changed = false;

    ImGui::PushID(id);

    const std::string typeLabel = (typeName && *typeName) ? std::string(typeName) : std::string("Game Object");
    const std::string emptyText = "None (" + typeLabel + ")";

    SceneObject* current = (targetId >= 0) ? findObjectById(targetId) : nullptr;
    if (targetId >= 0 && !current) {
        // The referenced SceneOBJ is gone - deleted, or the scene reloaded without it.
        targetId = -1;
        changed = true;
    }

    if (label && *label) {
        DrawInspectorInlineLabel(label, kReferenceSlotLabelWidth);
        ImGui::SameLine();
    }

    bool dropCandidate = false;
    if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload()) {
        if (dragging->IsDataType("SCENE_OBJECT") && dragging->DataSize == sizeof(int)) {
            dropCandidate = (*static_cast<const int*>(dragging->Data) != disallowId);
        }
    }

    const ReferenceSlotFrame frame = BeginReferenceSlotFrame("##Slot", dropCandidate);
    const char* popupId = "##ObjectReferenceSlotPicker";
    if (frame.pressed) {
        ImGui::OpenPopup(popupId);
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT")) {
            if (payload->DataSize == sizeof(int)) {
                const int droppedId = *static_cast<const int*>(payload->Data);
                if (droppedId != disallowId && droppedId != targetId) {
                    targetId = droppedId;
                    current = findObjectById(targetId);
                    changed = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Same component icon the Hierarchy row shows, custom editor icon included; an empty slot
    // wears a dimmed generic SceneOBJ icon so it still reads as a SceneOBJ field.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    bool iconDrawn = false;
    if (rendererInitialized) {
        Texture* icon = nullptr;
        if (current) {
            if (!current->editorIconPath.empty()) {
                icon = renderer.getTexture(current->editorIconPath,
                                           MaterialProperties::TextureFilter::Bilinear);
            }
            if (!icon || !icon->GetID()) {
                icon = renderer.getTexture(SceneObjectComponentIconPath(*current),
                                           MaterialProperties::TextureFilter::Bilinear);
            }
        } else {
            icon = renderer.getTexture("Resources/Engine-Root/Inspector/GameObject Icon.png",
                                       MaterialProperties::TextureFilter::Bilinear);
        }
        if (icon && icon->GetID()) {
            drawList->AddImage((ImTextureID)(intptr_t)icon->GetID(),
                               frame.iconMin, frame.iconMax, ImVec2(0, 1), ImVec2(1, 0),
                               current ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 96));
            iconDrawn = true;
        }
    }
    if (!iconDrawn) {
        DrawReferenceSlotIconPlaceholder(frame, current == nullptr);
    }

    const std::string displayName = current
        ? (current->name + " (" + std::to_string(current->id) + ")")
        : emptyText;
    DrawReferenceSlotText(frame, displayName, current == nullptr);

    if (frame.hovered) {
        if (current) {
            ImGui::SetTooltip("%s\nClick to pick another, right-click for more.", current->name.c_str());
        } else {
            ImGui::SetTooltip("Drag a SceneOBJ here, or click to pick one.");
        }
    }

    SceneObject* editorSelection = findObjectById(selectedObjectId);
    const bool canUseSelection = editorSelection && editorSelection->id != disallowId;
    if (ImGui::BeginPopupContextItem("##ObjectReferenceSlotMenu")) {
        ImGui::BeginDisabled(targetId < 0);
        if (ImGui::MenuItem("Clear")) {
            targetId = -1;
            changed = true;
        }
        if (ImGui::MenuItem("Select in Hierarchy")) {
            if (current) {
                setPrimarySelection(current->id);
            }
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(!canUseSelection);
        if (ImGui::MenuItem("Use Hierarchy Selection")) {
            targetId = editorSelection->id;
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup(popupId)) {
        static std::unordered_map<std::string, std::string> pickerFilters;
        std::string& filter = pickerFilters[std::string(id)];
        char searchBuf[128] = {};
        std::snprintf(searchBuf, sizeof(searchBuf), "%s", filter.c_str());
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::InputTextWithHint("##Filter", "Filter SceneOBJs", searchBuf, sizeof(searchBuf))) {
            filter = searchBuf;
        }

        if (ImGui::Selectable(emptyText.c_str(), targetId < 0)) {
            targetId = -1;
            changed = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::Separator();
        ImGui::BeginChild("##ObjectReferenceSlotList", ImVec2(320.0f, 180.0f), false);
        for (const SceneObject& candidate : sceneObjects) {
            if (candidate.id == disallowId) {
                continue;
            }
            const std::string option = candidate.name + " (" + std::to_string(candidate.id) + ")";
            if (!MatchesAssetFilter(option, filter)) {
                continue;
            }
            const bool selected = (candidate.id == targetId);
            if (ImGui::Selectable(option.c_str(), selected)) {
                targetId = candidate.id;
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    ImGui::PopID();
    return changed;
}
#pragma endregion

#pragma region Asset Drops
// Materials, scripts and audio clips dropped straight onto a SceneOBJ. One implementation for
// the Scene view, the Hierarchy and the Inspector, so the three can never disagree on what a
// drop does.
Engine::ObjectAssetDropKind Engine::classifyObjectAssetDrop(const char* rawPath) {
    if (!rawPath || !*rawPath) return ObjectAssetDropKind::None;
    // A drag reports the same path every frame and classifying it stats the file, so the last
    // answer is kept.
    if (assetDropKindCachePath == rawPath) return assetDropKindCache;
    assetDropKindCachePath = rawPath;
    assetDropKindCache = ObjectAssetDropKind::None;

    const fs::path path(rawPath);
    std::error_code ec;
    fs::directory_entry entry(path, ec);
    if (ec || !entry.is_regular_file(ec) || ec) return assetDropKindCache;

    const FileCategory category = fileBrowser.getFileCategory(entry);
    if (category == FileCategory::Material) {
        assetDropKindCache = ObjectAssetDropKind::Material;
    } else if (category == FileCategory::Audio) {
        assetDropKindCache = ObjectAssetDropKind::Audio;
    } else if (isNativeScriptSourcePath(path) || LowerExtension(path) == ".cs") {
        // Only what a Script component can run; a .h or .py is "Script" to the Project panel
        // but would just be a broken component here.
        assetDropKindCache = ObjectAssetDropKind::Script;
    }
    return assetDropKindCache;
}

Engine::ObjectAssetDropKind Engine::activeObjectAssetDragKind(std::string* outPath) {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (!payload || !payload->IsDataType("FILE_PATH") || !payload->Data) {
        return ObjectAssetDropKind::None;
    }
    const char* path = static_cast<const char*>(payload->Data);
    if (outPath) *outPath = path;
    return classifyObjectAssetDrop(path);
}

bool Engine::canApplyAssetDrop(const SceneObject& obj, ObjectAssetDropKind kind) const {
    switch (kind) {
        case ObjectAssetDropKind::Material:
            // Only what draws with obj.material. A UI Image keeps its picture in
            // albedoTexturePath, which loading a material would overwrite.
            return HasRendererComponent(obj);
        case ObjectAssetDropKind::Script:
        case ObjectAssetDropKind::Audio:
            return true;
        case ObjectAssetDropKind::None:
        default:
            return false;
    }
}

bool Engine::applyAssetDropToObject(SceneObject& obj, const fs::path& path, bool recordUndo) {
    const std::string pathString = path.string();
    const ObjectAssetDropKind kind = classifyObjectAssetDrop(pathString.c_str());
    if (!canApplyAssetDrop(obj, kind)) return false;

    const std::string targetName = obj.name.empty() ? std::string("Object") : obj.name;
    const std::string assetName = path.filename().string();
    switch (kind) {
        case ObjectAssetDropKind::Material: {
            if (materialPathsMatch(obj.materialPath, pathString)) return false;
            if (recordUndo) recordState("Assign Material");
            obj.materialPath = pathString;
            loadMaterialFromFile(obj);
            showEditorToast(assetName + " assigned to " + targetName + ".", ConsoleMessageType::Success, 1.65);
            break;
        }
        case ObjectAssetDropKind::Script: {
            const bool alreadyAssigned = std::any_of(obj.scripts.begin(), obj.scripts.end(),
                [&](const ScriptComponent& sc) { return sc.path == pathString; });
            if (alreadyAssigned) {
                showEditorToast(targetName + " already has " + assetName + ".", ConsoleMessageType::Info, 1.65);
                return false;
            }
            if (recordUndo) recordState("Add Script");
            ScriptComponent sc;
            sc.path = pathString;
            sc.lastBinaryPath.clear();
            sc.lastBinaryVerified = false;
            if (LowerExtension(path) == ".cs") {
                sc.language = ScriptLanguage::CSharp;
                sc.managedType = InferManagedTypeFromFile(path).value_or(path.stem().string());
            } else {
                sc.language = inferNativeLanguageFromPath(path);
            }
            obj.scripts.push_back(sc);
            EnsureInspectorComponentMetadata(obj);
            markRuntimeScriptBindingsDirty();
            playEditorSound(EditorSoundCue::ScriptAssigned, 0.95f);
            showEditorToast("Script assigned to " + targetName + " Successful.", ConsoleMessageType::Success, 1.65);
            addConsoleMessage("Assigned script to " + obj.name, ConsoleMessageType::Success);
            break;
        }
        case ObjectAssetDropKind::Audio: {
            if (obj.hasAudioSource && obj.audioSource.clipPath == pathString) return false;
            if (recordUndo) recordState("Assign Audio Clip");
            const bool addedSource = !obj.hasAudioSource;
            if (addedSource) {
                obj.hasAudioSource = true;
                obj.audioSource = AudioSourceComponent{};
                EnsureInspectorComponentMetadata(obj);
            }
            obj.audioSource.clipPath = pathString;
            showEditorToast(addedSource ? "Audio Source with " + assetName + " added to " + targetName + "."
                                        : assetName + " assigned to " + targetName + "'s Audio Source.",
                            ConsoleMessageType::Success, 1.65);
            addConsoleMessage("Assigned audio clip to " + obj.name, ConsoleMessageType::Success);
            break;
        }
        case ObjectAssetDropKind::None:
        default:
            return false;
    }
    projectManager.currentProject.hasUnsavedChanges = true;
    return true;
}

void Engine::setAssetDropHover(int objectId) {
    assetDropHoverObjectId = objectId;
    assetDropHoverFrame = ImGui::GetFrameCount();
}

// The Scene view renders before the panels that track the cursor run, so a hover set on the
// previous frame is still current. Anything older is a drag that has left every drop target.
int Engine::activeAssetDropHover() const {
    if (assetDropHoverObjectId < 0 || ImGui::GetDragDropPayload() == nullptr) return -1;
    return (ImGui::GetFrameCount() - assetDropHoverFrame) <= 1 ? assetDropHoverObjectId : -1;
}
#pragma endregion

#pragma region Inspector Panel
void Engine::renderInspectorPanel() {
    const auto __ipStart = std::chrono::steady_clock::now();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    const bool inspectorOpen = ImGui::Begin(Loc::Window("WINDOW_INSPECTOR", "Inspector"), &showInspector);
    ImGui::PopStyleVar();
    if (!inspectorOpen) {
        ImGui::End();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 2.0f));

    if (deferInspectorRefresh) {
        deferInspectorRefresh = false;
        ImGui::TextDisabled("Refreshing inspector after play mode transition...");
        ImGui::PopStyleVar(3);
        ImGui::End();
        return;
    }

    const bool inspectorWrapText =
        (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_IsTouchScreen) != 0;
    if (inspectorWrapText) {
        ImGui::PushTextWrapPos(0.0f);
    }

    fs::path selectedMaterialPath;
    bool browserHasMaterial = false;
    fs::path selectedAudioPath;
    bool browserHasAudio = false;
    const AudioClipPreview* selectedAudioPreview = nullptr;
    fs::path selectedTexturePath;
    bool browserHasTexture = false;
    fs::path selectedVideoPath;
    bool browserHasVideo = false;
    static std::unordered_map<int, int> selectedRendererMaterialSlots;
    static std::string slotMaterialInspectorPath;
    static bool slotMaterialInspectorValid = false;
    static MaterialProperties slotInspectedMaterial;
    static std::string slotInspectedAlbedo;
    static std::string slotInspectedOverlay;
    static std::string slotInspectedNormal;
    static std::string slotInspectedMetallicRoughness;
    static std::string slotInspectedMetallicMap;
    static std::string slotInspectedRoughnessMap;
    static std::string slotInspectedOpacityMap;
    static std::string slotInspectedEmissive;
    static std::string slotInspectedOcclusion;
    static std::string slotInspectedHeight;
    static std::string slotInspectedShaderPack;
    static std::string slotInspectedVertShader;
    static std::string slotInspectedFragShader;
    static bool slotInspectedUseOverlay = false;
    if (!fileBrowser.selectedFile.empty() && fs::exists(fileBrowser.selectedFile)) {
        fs::directory_entry entry(fileBrowser.selectedFile);
        FileCategory cat = fileBrowser.getFileCategory(entry);
        if (cat == FileCategory::Material) {
            selectedMaterialPath = entry.path();
            browserHasMaterial = true;
            if (inspectedMaterialPath != selectedMaterialPath.string()) {
                inspectedMaterialValid = loadMaterialData(
                    selectedMaterialPath.string(),
                    inspectedMaterial,
                    inspectedAlbedo,
                    inspectedOverlay,
                    inspectedNormal,
                    inspectedUseOverlay,
                    &inspectedShaderPack,
                    &inspectedVertShader,
                    &inspectedFragShader,
                    &inspectedMetallicRoughness,
                    &inspectedMetallicMap,
                    &inspectedRoughnessMap,
                    &inspectedOpacityMap,
                    &inspectedEmissive,
                    &inspectedOcclusion,
                    &inspectedHeight
                );
                inspectedMaterialPath = selectedMaterialPath.string();
            }
        } else {
            inspectedMaterialPath.clear();
            inspectedMaterialValid = false;
            inspectedShaderPack.clear();
        }
        if (cat == FileCategory::Audio) {
            selectedAudioPath = entry.path();
            selectedAudioPreview = nullptr;
            browserHasAudio = false;

            if (!selectedAudioPath.empty()) {
                selectedAudioPreview = audio.getPreview(selectedAudioPath.string());
                browserHasAudio = (selectedAudioPreview != nullptr);
            }
        }
        if (cat == FileCategory::Texture) {
            selectedTexturePath = entry.path();
            browserHasTexture = true;
        }
        if (cat == FileCategory::Video) {
            selectedVideoPath = entry.path();
            browserHasVideo = true;
        }
    } else {
        inspectedMaterialPath.clear();
        inspectedMaterialValid = false;
        inspectedShaderPack.clear();
    }

    if (browserHasAudio) {
        std::string selectedAudio = selectedAudioPath.string();
        if (selectedAudio != audioPreviewSelectedPath) {
            audioPreviewSelectedPath = selectedAudio;
            if (audioPreviewAutoPlay) {
                audioPreviewBaseVolume = 1.0f;
                audioPreviewContext = AudioPreviewContext::AssetBrowser;
                audio.playPreview(selectedAudio, audioPreviewBaseVolume * audioPreviewVolume, audioPreviewLoop);
            }
        }
    } else {
        audioPreviewSelectedPath.clear();
    }

    const bool assetPreviewSuppressed = isPlaying || specMode || testMode || playerMode;
    const bool shouldPreviewVideoAsset = !assetPreviewSuppressed && browserHasVideo && selectedObjectIds.empty();
    if (!shouldPreviewVideoAsset) {
        if (videoAssetPreviewPlayer) {
            videoAssetPreviewPlayer.reset();
            videoAssetPreviewPath.clear();
        }
    } else {
        const fs::path resolvedVideoPath = resolveProjectAssetPath(selectedVideoPath.string());
        const std::string resolvedVideoPathString = resolvedVideoPath.string();
        if (videoAssetPreviewPath != resolvedVideoPathString) {
            videoAssetPreviewPlayer.reset();
            videoAssetPreviewPath = resolvedVideoPathString;
        }

        if (videoAssetPreviewPlayer) {
            videoAssetPreviewPlayer->SetLoop(videoAssetPreviewLoop);
            videoAssetPreviewPlayer->Update(videoAssetPreviewPlayer->IsPlaying() ? deltaTime : 0.0f);
        }
    }

    auto drawWaveform = [&](const char* id, const AudioClipPreview* preview, const ImVec2& size, float progressRatio, float* seekRatioOut) {
        bool hasStereo = preview && preview->channels >= 2
            && !preview->waveformLeft.empty()
            && !preview->waveformRight.empty();
        if (!preview || (!hasStereo && preview->waveform.empty())) {
            ImGui::Dummy(size);
            return;
        }
        ImVec2 start = ImGui::GetCursorScreenPos();
        ImVec2 end = ImVec2(start.x + size.x, start.y + size.y);
        ImGui::InvisibleButton(id, size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(start, end, IM_COL32(30, 35, 45, 180), 4.0f);
        float midY = (start.y + end.y) * 0.5f;
        float usableHeight = size.y * 0.45f;
        size_t count = hasStereo
            ? std::min(preview->waveformLeft.size(), preview->waveformRight.size())
            : preview->waveform.size();
        float step = count > 1 ? size.x / static_cast<float>(count - 1) : size.x;
        if (hasStereo) {
            ImU32 leftColor = IM_COL32(255, 190, 90, 200);
            ImU32 rightColor = IM_COL32(100, 200, 255, 200);
            float topMidY = start.y + size.y * 0.25f;
            float bottomMidY = start.y + size.y * 0.75f;
            float stereoHeight = size.y * 0.22f;
            for (size_t i = 0; i < count; ++i) {
                float leftAmp = std::clamp(preview->waveformLeft[i], 0.0f, 1.0f);
                float rightAmp = std::clamp(preview->waveformRight[i], 0.0f, 1.0f);
                float x = start.x + step * static_cast<float>(i);
                float leftOff = leftAmp * stereoHeight;
                float rightOff = rightAmp * stereoHeight;
                dl->AddLine(ImVec2(x, topMidY - leftOff), ImVec2(x, topMidY + leftOff), leftColor, 1.2f);
                dl->AddLine(ImVec2(x, bottomMidY - rightOff), ImVec2(x, bottomMidY + rightOff), rightColor, 1.2f);
            }
        } else {
            ImU32 color = IM_COL32(255, 180, 100, 200);
            for (size_t i = 0; i < count; ++i) {
                float amp = std::clamp(preview->waveform[i], 0.0f, 1.0f);
                float x = start.x + step * static_cast<float>(i);
                float yOff = amp * usableHeight;
                dl->AddLine(ImVec2(x, midY - yOff), ImVec2(x, midY + yOff), color, 1.2f);
            }
        }

        if (progressRatio >= 0.0f && progressRatio <= 1.0f) {
            float px = start.x + progressRatio * size.x;
            dl->AddLine(ImVec2(px, start.y), ImVec2(px, end.y), IM_COL32(120, 210, 255, 230), 2.0f);
        }

        if (seekRatioOut && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            float mouseX = ImGui::GetIO().MousePos.x;
            float ratio = (mouseX - start.x) / size.x;
            ratio = std::clamp(ratio, 0.0f, 1.0f);
            *seekRatioOut = ratio;
        }
    };

    struct AudioPlayerUiIcon {
        ImTextureID id = static_cast<ImTextureID>(0);
        bool flipY = false;
        float width = 0.0f;
        float height = 0.0f;
    };

    const bool hasVulkanUiImages = usingVulkan() && vulkanRendererInitialized && (vulkanRenderer != nullptr);
    auto resolveAudioPlayerIcon = [&](const char* iconPath,
                                      MaterialProperties::TextureFilter filter =
                                          MaterialProperties::TextureFilter::Point) -> AudioPlayerUiIcon {
        if (!iconPath || !*iconPath) {
            return {};
        }
        if (rendererInitialized) {
            if (Texture* icon = renderer.getTexture(iconPath, filter);
                icon && icon->GetID()) {
                return { static_cast<ImTextureID>(icon->GetID()),
                         true,
                         static_cast<float>(icon->GetWidth()),
                         static_cast<float>(icon->GetHeight()) };
            }
        }
        if (hasVulkanUiImages && vulkanRenderer) {
            ImTextureID icon = vulkanRenderer->getOrCreateUIImage(iconPath);
            if (icon != static_cast<ImTextureID>(0)) {
                return { icon, false, 0.0f, 0.0f };
            }
        }
        return {};
    };

    struct InspectorUiIcon {
        ImTextureID id = static_cast<ImTextureID>(0);
        bool flipY = false;
        float nativeW = 0.0f;
        float nativeH = 0.0f;
    };

    auto resolveInspectorIcon = [&](const char* iconPath,
                                    MaterialProperties::TextureFilter filter =
                                        MaterialProperties::TextureFilter::Point) -> InspectorUiIcon {
        if (!iconPath || !*iconPath) return {};
        if (rendererInitialized) {
            if (Texture* icon = renderer.getTexture(iconPath, filter);
                icon && icon->GetID()) {
                return { static_cast<ImTextureID>(icon->GetID()), true,
                         static_cast<float>(icon->GetWidth()),
                         static_cast<float>(icon->GetHeight()) };
            }
        }
        if (hasVulkanUiImages && vulkanRenderer) {
            ImTextureID icon = vulkanRenderer->getOrCreateUIImage(iconPath);
            if (icon != static_cast<ImTextureID>(0)) return { icon, false, 0.0f, 0.0f };
        }
        return {};
    };

    const InspectorUiIcon iconGameObject  = resolveInspectorIcon("Resources/Engine-Root/Inspector/GameObject Icon.png");
    const InspectorUiIcon iconTransform   = resolveInspectorIcon("Resources/Engine-Root/Inspector/Transform Component Icon.png");
    const InspectorUiIcon iconScript      = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Script.png");
    const InspectorUiIcon iconActionsMenu = resolveInspectorIcon("Resources/Engine-Root/Inspector/Tab Area/New/Actions (... menu).png");
    const InspectorUiIcon iconCollapseArrow = resolveInspectorIcon(
        "Resources/Engine-Root/Inspector/Tab Area/New/Collapse Button Collapsed.png");
    const InspectorUiIcon iconUnknownComponent = resolveInspectorIcon(
        "Resources/Engine-Root/Inspector/Tab Area/New/Unknown Component Icon.png");
    const InspectorUiIcon iconInvariableOff = resolveInspectorIcon(
        "Resources/Engine-Root/Inspector/Invariable Switch Off.png",
        MaterialProperties::TextureFilter::Bilinear);
    const InspectorUiIcon iconInvariableOn = resolveInspectorIcon(
        "Resources/Engine-Root/Inspector/Invariable Switch On.png",
        MaterialProperties::TextureFilter::Bilinear);
    const InspectorUiIcon iconTextureSelect = resolveInspectorIcon(
        "Resources/Engine-Root/Inspector/Materials and texturing/Select Texture list icon.png");
    const InspectorUiIcon iconColorPicker = resolveInspectorIcon(
        "Resources/Engine-Root/Inspector/Materials and texturing/Color Picker Icon.png");
    (void)iconTransform;

    const InspectorUiIcon iconCompCamera = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Component Camera.png");
    const InspectorUiIcon iconCompLight  = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Component Light Bulb.png");
    const InspectorUiIcon iconCompVolume = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Component ModuVolume.png");
    const InspectorUiIcon iconCompAudio  = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Component Audio Source.png");
    const InspectorUiIcon iconCompMesh   = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Component Mesh.png");
    const InspectorUiIcon iconCompCanvas = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Component Canvas.png");
    const InspectorUiIcon iconCompText   = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Component Text.png");
    const InspectorUiIcon iconCompSprite = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Component Sprite.png");
    const InspectorUiIcon iconCompCollider  = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Collider any type.png");
    const InspectorUiIcon iconCompRigidbody = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Rigidbody 2D and 3D type.png");
    const InspectorUiIcon iconCompParticle  = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Particle System any type.png");
    const InspectorUiIcon iconCompVideo     = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Video Player.png");
    const InspectorUiIcon iconCompScript    = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Script.png");
    const InspectorUiIcon iconCompNetwork   = resolveInspectorIcon("Resources/Engine-Root/Hierarchy/Network System Logo.png");

    auto formatAudioClock = [](double seconds, bool roundUp) -> std::string {
        if (!std::isfinite(seconds) || seconds <= 0.0) {
            return "0:00";
        }

        const double quantized = roundUp
            ? std::ceil(std::max(0.0, seconds) - 0.0001)
            : std::floor(std::max(0.0, seconds) + 0.0001);
        const long long totalSeconds = static_cast<long long>(std::max(0.0, quantized));
        const long long hours = totalSeconds / 3600;
        const long long minutes = (totalSeconds / 60) % 60;
        const long long secs = totalSeconds % 60;

        std::ostringstream out;
        out << std::setfill('0');
        if (hours > 0) {
            out << hours << ':' << std::setw(2) << minutes << ':' << std::setw(2) << secs;
        } else {
            out << (totalSeconds / 60) << ':' << std::setw(2) << secs;
        }
        return out.str();
    };

    auto drawAudioTimeReadout = [&](double cursorSeconds, double durationSeconds) {
        const double safeCursor = std::max(0.0, cursorSeconds);
        const double safeDuration = std::max(0.0, durationSeconds);
        const std::string currentClock = formatAudioClock(safeCursor, false);
        const std::string durationClock = formatAudioClock(safeDuration, true);
        ImGui::TextColored(ImVec4(0.98f, 0.82f, 0.55f, 1.0f), "%s / %s", currentClock.c_str(), durationClock.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("Timing: %.2fs / %.2fs", safeCursor, safeDuration);
    };

    auto drawTrimmedPathText = [&](const std::string& path, const ImVec4& color) {
        const float maxWidth = std::max(32.0f, ImGui::GetContentRegionAvail().x);
        std::string display = path;
        if (ImGui::CalcTextSize(display.c_str()).x > maxWidth) {
            constexpr const char* kEllipsis = "...";
            std::string suffix = path;
            while (!suffix.empty()) {
                std::string candidate = std::string(kEllipsis) + suffix;
                if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth) {
                    display = std::move(candidate);
                    break;
                }
                suffix.erase(suffix.begin());
            }
            if (suffix.empty()) {
                display = kEllipsis;
            }
        }

        ImGui::TextColored(color, "%s", display.c_str());
        if (display != path && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", path.c_str());
        }
    };

    auto fitLabelToWidth = [](const std::string& text, float maxWidth) {
        return FitLabelToWidth(text, maxWidth);
    };

    auto drawAudioCenteredText = [&](const std::string& text,
                                     const ImVec4* color,
                                     const char* tooltip = nullptr) {
        const float avail = std::max(12.0f, ImGui::GetContentRegionAvail().x);
        const std::string display = fitLabelToWidth(text, avail);
        const float textWidth = ImGui::CalcTextSize(display.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - textWidth) * 0.5f));
        if (color) {
            ImGui::TextColored(*color, "%s", display.c_str());
        } else {
            ImGui::TextDisabled("%s", display.c_str());
        }
        if (ImGui::IsItemHovered()) {
            if (tooltip && *tooltip) {
                ImGui::SetTooltip("%s", tooltip);
            } else if (display != text) {
                ImGui::SetTooltip("%s", text.c_str());
            }
        }
    };

    auto drawMaterialInlineLabel = [](const char* label, float width = 126.0f) {
        DrawInspectorInlineLabel(label, width);
    };

    auto drawAudioPlayerIconButton = [&](const char* id,
                                         const char* iconPath,
                                         const char* fallbackText,
                                         const char* tooltip,
                                         bool active,
                                         bool disabled,
                                         const ImVec2& size,
                                         const ImVec4& accentColor) -> bool {
        if (disabled) {
            ImGui::BeginDisabled();
        }

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, size);
        if (pressed) ImGui::ReportWidgetFeedback(ImGuiWidgetFeedback_Button);
        ImGui::ReportItemHoverFeedback();
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const ImVec2 max(pos.x + size.x, pos.y + size.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float rounding = 10.0f;

        ImVec4 bg = active
            ? ImVec4(accentColor.x * 0.70f, accentColor.y * 0.70f, accentColor.z * 0.70f, 0.95f)
            : hovered
                ? ImVec4(0.24f, 0.27f, 0.35f, 0.95f)
                : ImVec4(0.15f, 0.17f, 0.22f, 0.92f);
        ImVec4 border = active
            ? ImVec4(accentColor.x, accentColor.y, accentColor.z, 1.0f)
            : hovered
                ? ImVec4(0.58f, 0.66f, 0.78f, 0.95f)
                : ImVec4(0.28f, 0.32f, 0.41f, 0.92f);
        if (held) {
            bg = ImVec4(bg.x + 0.05f, bg.y + 0.05f, bg.z + 0.05f, bg.w);
        }

        if (disabled) {
            bg.w *= 0.55f;
            border.w *= 0.45f;
        }

        drawList->AddRectFilled(pos, max, ImGui::ColorConvertFloat4ToU32(bg), rounding);
        drawList->AddRect(pos, max, ImGui::ColorConvertFloat4ToU32(border), rounding, 0, active ? 2.0f : 1.0f);

        const AudioPlayerUiIcon icon = resolveAudioPlayerIcon(iconPath);
        const float inset = size.x >= 40.0f ? 8.0f : 6.0f;
        float iconW = size.x - inset * 2.0f;
        float iconH = size.y - inset * 2.0f;
        if (icon.width > 0.0f && icon.height > 0.0f && icon.width <= size.x && icon.height <= size.y) {
            iconW = icon.width;
            iconH = icon.height;
        }
        const ImVec2 iconMin(std::round(pos.x + (size.x - iconW) * 0.5f),
                             std::round(pos.y + (size.y - iconH) * 0.5f));
        const ImVec2 iconMax(iconMin.x + iconW, iconMin.y + iconH);
        const int alpha = disabled ? 110 : active ? 255 : hovered ? 240 : 215;
        if (icon.id != static_cast<ImTextureID>(0)) {
            const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
            const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
            drawList->AddImage(icon.id, iconMin, iconMax, uvMin, uvMax, IM_COL32(255, 255, 255, alpha));
        } else if (fallbackText && *fallbackText) {
            const ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
            drawList->AddText(
                ImVec2(pos.x + (size.x - textSize.x) * 0.5f, pos.y + (size.y - textSize.y) * 0.5f),
                IM_COL32(255, 255, 255, alpha),
                fallbackText);
        }

        if (hovered && tooltip && *tooltip) {
            ImGui::SetTooltip("%s", tooltip);
        }

        if (disabled) {
            ImGui::EndDisabled();
        }
        return !disabled && pressed;
    };

    auto stopClipPreview = [&]() {
        audio.stopPreview();
        audioPreviewBaseVolume = 1.0f;
        audioPreviewContext = AudioPreviewContext::None;
    };

    auto beginClipPreview = [&](const std::string& path, float baseVolume, bool loop, AudioPreviewContext context) {
        audioPreviewBaseVolume = std::max(0.0f, baseVolume);
        audioPreviewContext = context;
        return audio.playPreview(path, audioPreviewBaseVolume * audioPreviewVolume, loop);
    };

    auto syncClipPreviewVolume = [&](AudioPreviewContext context, float baseVolume) {
        if (audioPreviewContext != context) {
            return;
        }
        audioPreviewBaseVolume = std::max(0.0f, baseVolume);
        audio.setPreviewVolume(audioPreviewBaseVolume * audioPreviewVolume);
    };

    auto drawAudioPreviewVolumeControl = [&](const char* id, AudioPreviewContext context, float baseVolume) {
        const AudioPlayerUiIcon icon = resolveAudioPlayerIcon(
            "Resources/Engine-Root/Audio Player/Audio Icon.png",
            MaterialProperties::TextureFilter::Bilinear);
        const char* caption = "Preview Volume";
        const float iconSize = 22.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float captionWidth = ImGui::CalcTextSize(caption).x;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float sliderWidth = std::max(90.0f, std::min(190.0f, avail - iconSize - captionWidth - spacing * 2.0f));
        const float rowWidth = iconSize + captionWidth + sliderWidth + spacing * 2.0f;

        const float rowStart = ImGui::GetCursorPosX() + std::max(0.0f, (avail - rowWidth) * 0.5f);
        ImGui::SetCursorPosX(rowStart);

        const float rowHeight = std::max(iconSize, ImGui::GetFrameHeight());
        const float iconOffset = std::max(0.0f, (rowHeight - iconSize) * 0.5f);
        if (icon.id != static_cast<ImTextureID>(0)) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + iconOffset);
            const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
            const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
            ImGui::Image(icon.id, ImVec2(iconSize, iconSize), uvMin, uvMax);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconOffset);
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Vol");
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", caption);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sliderWidth);
        if (ImGui::SliderFloat(id, &audioPreviewVolume, 0.0f, 2.0f, "%.2fx")) {
            audioPreviewVolume = std::clamp(audioPreviewVolume, 0.0f, 2.0f);
            saveEditorUserSettings();
            syncClipPreviewVolume(context, baseVolume);
        }
    };

    auto smallButtonRunWidth = [](std::initializer_list<const char*> labels) -> float {
        const ImGuiStyle& style = ImGui::GetStyle();
        float total = 0.0f;
        for (const char* label : labels) {
            total += style.ItemSpacing.x +
                     ImGui::CalcTextSize(label, nullptr, true).x +
                     style.FramePadding.x * 2.0f;
        }
        return total;
    };
    auto fieldWidthBeforeButtons = [](float buttonsWidth,
                                      float minFieldWidth = 90.0f) -> bool {
        const bool fits =
            (ImGui::GetContentRegionAvail().x - buttonsWidth) >= minFieldWidth;
        ImGui::SetNextItemWidth(fits ? -buttonsWidth : -FLT_MIN);
        return fits;
    };

    // The material preview is a full off-screen scene render (four objects, its own lights). It
    // only changes when its inputs change, but it used to be re-rendered every frame an object was
    // selected, which is most of what the Inspector cost. The referenced texture ids are part of
    // the signature so a preview still refreshes once a texture finishes loading. This mirrors the
    // file browser's thumbnail cache.
    auto previewInputSignature = [&](const MaterialProperties& material,
                                     const std::string& albedoPath,
                                     const std::string& overlayPath,
                                     const std::string& normalPath,
                                     bool useOverlay,
                                     const std::string& vertShaderPath,
                                     const std::string& fragShaderPath,
                                     int targetWidth,
                                     int targetHeight,
                                     const glm::vec2& orbitAngles) {
        auto mix64 = [](uint64_t seed, uint64_t value) {
            return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
        };
        auto mixFloat = [&](uint64_t seed, float value) {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return mix64(seed, bits);
        };
        auto mixString = [&](uint64_t seed, const std::string& value) {
            return mix64(seed, static_cast<uint64_t>(std::hash<std::string>{}(value)));
        };
        auto mixTexture = [&](uint64_t seed, const std::string& path) {
            if (path.empty()) return mix64(seed, 0ull);
            Texture* tex = renderer.getTexture(path, material.textureFilter);
            return mix64(seed, static_cast<uint64_t>(tex ? tex->GetID() : 0u));
        };

        // MaterialProperties holds no heap members, so its bytes cover every field that
        // reaches the preview shader without a list here to keep in sync.
        static_assert(std::is_trivially_copyable<MaterialProperties>::value,
                      "Material preview signature hashes MaterialProperties byte-wise");
        uint64_t seed = 1469598103934665603ull;
        std::array<unsigned char, sizeof(MaterialProperties)> materialBytes{};
        std::memcpy(materialBytes.data(), &material, sizeof(MaterialProperties));
        for (unsigned char byte : materialBytes) {
            seed = mix64(seed, byte);
        }
        seed = mixString(seed, albedoPath);
        seed = mixString(seed, overlayPath);
        seed = mixString(seed, normalPath);
        seed = mixString(seed, vertShaderPath);
        seed = mixString(seed, fragShaderPath);
        seed = mix64(seed, useOverlay ? 2ull : 1ull);
        seed = mix64(seed, static_cast<uint64_t>(targetWidth));
        seed = mix64(seed, static_cast<uint64_t>(targetHeight));
        seed = mixFloat(seed, orbitAngles.x);
        seed = mixFloat(seed, orbitAngles.y);
        seed = mixTexture(seed, albedoPath);
        seed = mixTexture(seed, overlayPath);
        seed = mixTexture(seed, normalPath);
        return seed;
    };

    auto renderMaterialPreviewTexture = [&](const MaterialProperties& material,
                                            const std::string& albedoPath,
                                            const std::string& overlayPath,
                                            const std::string& normalPath,
                                            bool useOverlay,
                                            const std::string& vertShaderPath,
                                            const std::string& fragShaderPath,
                                            int targetWidth,
                                            int targetHeight,
                                            int previewSlot,
                                            const glm::vec2& orbitAngles) {
        static const std::string kPreviewWhiteTexture = "Resources/Textures/editor_preview_white.ppm";

        struct PreviewCacheEntry {
            uint64_t signature = 0;
            unsigned int texture = 0;
        };
        static std::unordered_map<int, PreviewCacheEntry> sPreviewCacheBySlot;
        const uint64_t signature = previewInputSignature(
            material, albedoPath, overlayPath, normalPath, useOverlay,
            vertShaderPath, fragShaderPath, targetWidth, targetHeight, orbitAngles);
        PreviewCacheEntry& cacheEntry = sPreviewCacheBySlot[previewSlot];
        if (cacheEntry.texture != 0 && cacheEntry.signature == signature) {
            return cacheEntry.texture;
        }

        Camera previewCamera;
        previewCamera.position = glm::vec3(0.0f, 0.18f, 2.9f);
        previewCamera.front = glm::normalize(glm::vec3(0.0f, -0.06f, -1.0f));
        previewCamera.up = glm::vec3(0.0f, 1.0f, 0.0f);

        std::vector<SceneObject> previewScene;
        previewScene.reserve(4);

        SceneObject keyLight("MatPreviewKey", ObjectType::PointLight, -9201);
        keyLight.hasLight = true;
        keyLight.position = glm::vec3(1.5f, 1.6f, 2.0f);
        keyLight.light.type = LightType::Point;
        keyLight.light.color = glm::vec3(1.0f, 0.98f, 0.94f);
        keyLight.light.intensity = 2.8f;
        keyLight.light.range = 8.0f;
        previewScene.push_back(keyLight);

        SceneObject fillLight("MatPreviewFill", ObjectType::PointLight, -9202);
        fillLight.hasLight = true;
        fillLight.position = glm::vec3(-1.7f, 0.5f, 1.4f);
        fillLight.light.type = LightType::Point;
        fillLight.light.color = glm::vec3(0.72f, 0.79f, 1.0f);
        fillLight.light.intensity = 1.0f;
        fillLight.light.range = 8.0f;
        previewScene.push_back(fillLight);

        SceneObject rimLight("MatPreviewRim", ObjectType::DirectionalLight, -9203);
        rimLight.hasLight = true;
        rimLight.light.type = LightType::Directional;
        rimLight.light.color = glm::vec3(0.96f, 0.98f, 1.0f);
        rimLight.light.intensity = 0.55f;
        rimLight.rotation = glm::vec3(24.0f, 208.0f, 0.0f);
        previewScene.push_back(rimLight);

        SceneObject previewSphere("MatPreviewSphere", ObjectType::Sphere, -9204);
        previewSphere.hasRenderer = true;
        previewSphere.renderType = RenderType::Sphere;
        previewSphere.position = glm::vec3(0.0f, 0.0f, 0.0f);
        previewSphere.rotation = glm::vec3(std::clamp(-orbitAngles.y, -55.0f, 55.0f), orbitAngles.x - 18.0f, 0.0f);
        previewSphere.scale = glm::vec3(1.45f);
        previewSphere.material = material;
        previewSphere.albedoTexturePath = albedoPath.empty() ? kPreviewWhiteTexture : albedoPath;
        previewSphere.overlayTexturePath = overlayPath;
        previewSphere.normalMapPath = normalPath;
        previewSphere.useOverlay = useOverlay;
        previewSphere.vertexShaderPath = vertShaderPath;
        previewSphere.fragmentShaderPath = fragShaderPath;
        previewScene.push_back(previewSphere);

        const unsigned int previewTexture = renderer.renderScenePreview(
            previewCamera,
            previewScene,
            targetWidth,
            targetHeight,
            32.0f,
            0.1f,
            20.0f,
            false,
            previewSlot,
            true
        );
        cacheEntry.signature = signature;
        cacheEntry.texture = previewTexture;
        return previewTexture;
    };

    auto drawMaterialPreview = [&](const char* idSuffix,
                                   const MaterialProperties& material,
                                   const std::string& albedoPath,
                                   const std::string& overlayPath,
                                   const std::string& normalPath,
                                   bool useOverlay,
                                   const std::string& vertShaderPath,
                                   const std::string& fragShaderPath,
                                   float previewScale,
                                   int previewSlot) {
        static std::unordered_map<ImGuiID, ImVec2> previewOrbitById;

        ImGui::PushID(idSuffix);
        const ImGuiID previewId = ImGui::GetID("MaterialPreviewOrbit");
        ImVec2& orbit = previewOrbitById[previewId];

        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float previewWidth = std::clamp(212.0f * previewScale, 132.0f, std::max(132.0f, availableWidth));
        const float previewHeight = std::clamp(previewWidth * 0.76f, 126.0f, 240.0f);
        const int targetWidth = std::max(96, static_cast<int>(previewWidth));
        const int targetHeight = std::max(96, static_cast<int>(previewHeight));

        const unsigned int previewTexture = renderMaterialPreviewTexture(
            material,
            albedoPath,
            overlayPath,
            normalPath,
            useOverlay,
            vertShaderPath,
            fragShaderPath,
            targetWidth,
            targetHeight,
            previewSlot,
            glm::vec2(orbit.x, orbit.y)
        );

        const ImVec2 imageSize(previewWidth, previewHeight);
        const float padX = std::max(0.0f, availableWidth - previewWidth);
        if (padX > 1.0f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX * 0.5f);
        }

        const ImVec2 imagePos = ImGui::GetCursorScreenPos();
        if (previewTexture != 0) {
            ImGui::Image((ImTextureID)(intptr_t)previewTexture, imageSize, ImVec2(0, 1), ImVec2(1, 0));
            ImGui::SetCursorScreenPos(imagePos);
            ImGui::InvisibleButton("##PreviewDragArea", imageSize);
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 dragDelta = ImGui::GetIO().MouseDelta;
                orbit.x += dragDelta.x * 0.6f;
                orbit.y = std::clamp(orbit.y - dragDelta.y * 0.45f, -55.0f, 55.0f);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Drag to rotate preview");
            }
        } else {
            ImGui::Dummy(imageSize);
            ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 12.0f, imagePos.y + 12.0f),
                                                IM_COL32(255, 140, 140, 255), "Preview unavailable");
        }

        ImGui::PopID();
    };

    static float assetMaterialPreviewScale = 1.0f;
    static float objectMaterialPreviewScale = 1.0f;
    static float uiImageMaterialPreviewScale = 1.0f;

    struct ComponentHeaderState {
        bool open = false;
        bool enabledChanged = false;
    };

    enum class MaterialShaderPreset : int {
        Custom = 0,
        EngineLit = 1,
        StandardUnlit = 2,
        ScrollingUV = 3,
        ProceduralClouds = 4
    };

    auto resolveStandardUnlitShaderPaths = [&]() {
        std::string fragPath = "Resources/Shaders/standard_unlit_frag.glsl";
        if (projectManager.currentProject.isLoaded) {
            fs::path projFrag = projectManager.currentProject.assetsPath / "Shaders" / "standard_unlit_frag.glsl";
            std::error_code ec;
            if (fs::exists(projFrag, ec) && !ec) {
                fragPath = projFrag.string();
            }
        }
        return std::pair<std::string, std::string>{std::string(), fragPath};
    };

    auto resolveScrollingShaderPaths = [&]() {
        std::string vertPath = "Resources/Shaders/scroll_texture_vert.glsl";
        std::string fragPath = "Resources/Shaders/scroll_texture_frag.glsl";
        if (projectManager.currentProject.isLoaded) {
            fs::path projVert = projectManager.currentProject.assetsPath / "Shaders" / "scroll_texture_vert.glsl";
            fs::path projFrag = projectManager.currentProject.assetsPath / "Shaders" / "scroll_texture_frag.glsl";
            std::error_code ec;
            if (fs::exists(projVert, ec) && !ec) {
                vertPath = projVert.string();
            }
            ec.clear();
            if (fs::exists(projFrag, ec) && !ec) {
                fragPath = projFrag.string();
            }
        }
        return std::pair<std::string, std::string>{vertPath, fragPath};
    };

    auto resolveCloudsShaderPaths = [&]() {
        std::string vertPath = "Resources/Shaders/clouds_vert.glsl";
        std::string fragPath = "Resources/Shaders/clouds_frag.glsl";
        if (projectManager.currentProject.isLoaded) {
            fs::path projVert = projectManager.currentProject.assetsPath / "Shaders" / "clouds_vert.glsl";
            fs::path projFrag = projectManager.currentProject.assetsPath / "Shaders" / "clouds_frag.glsl";
            std::error_code ec;
            if (fs::exists(projVert, ec) && !ec) {
                vertPath = projVert.string();
            }
            ec.clear();
            if (fs::exists(projFrag, ec) && !ec) {
                fragPath = projFrag.string();
            }
        }
        return std::pair<std::string, std::string>{vertPath, fragPath};
    };

    auto shaderPresetFromPaths = [](const std::string& vert, const std::string& frag) {
        if (vert.empty() && frag.empty()) {
            return MaterialShaderPreset::EngineLit;
        }
        std::string vertFile = fs::path(vert).filename().string();
        std::string fragFile = fs::path(frag).filename().string();
        std::transform(vertFile.begin(), vertFile.end(), vertFile.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(fragFile.begin(), fragFile.end(), fragFile.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (fragFile == "standard_unlit_frag.glsl") {
            return MaterialShaderPreset::StandardUnlit;
        }
        if (vertFile == "scroll_texture_vert.glsl" && fragFile == "scroll_texture_frag.glsl") {
            return MaterialShaderPreset::ScrollingUV;
        }
        if (vertFile == "clouds_vert.glsl" && fragFile == "clouds_frag.glsl") {
            return MaterialShaderPreset::ProceduralClouds;
        }
        return MaterialShaderPreset::Custom;
    };

    auto shaderUsesCloudControls = [&](const std::string& vert, const std::string& frag) {
        if (shaderPresetFromPaths(vert, frag) == MaterialShaderPreset::ProceduralClouds) {
            return true;
        }
        std::string vertFile = MaterialAssetLowercase(fs::path(vert).filename().string());
        std::string fragFile = MaterialAssetLowercase(fs::path(frag).filename().string());
        return vertFile.find("cloud") != std::string::npos ||
               fragFile.find("cloud") != std::string::npos;
    };

    auto applyShaderPreset = [&](MaterialShaderPreset preset,
                                 std::string& shaderPack,
                                 std::string& vert,
                                 std::string& frag) {
        std::string nextVert = vert;
        std::string nextFrag = frag;
        std::string nextPack = shaderPack;
        if (preset == MaterialShaderPreset::EngineLit) {
            nextPack.clear();
            nextVert.clear();
            nextFrag.clear();
        } else if (preset == MaterialShaderPreset::StandardUnlit) {
            auto standardUnlit = resolveStandardUnlitShaderPaths();
            nextPack.clear();
            nextVert = standardUnlit.first;
            nextFrag = standardUnlit.second;
        } else if (preset == MaterialShaderPreset::ScrollingUV) {
            auto scrolling = resolveScrollingShaderPaths();
            nextPack.clear();
            nextVert = scrolling.first;
            nextFrag = scrolling.second;
        } else if (preset == MaterialShaderPreset::ProceduralClouds) {
            auto clouds = resolveCloudsShaderPaths();
            nextPack.clear();
            nextVert = clouds.first;
            nextFrag = clouds.second;
        }
        bool changed = (nextPack != shaderPack) || (nextVert != vert) || (nextFrag != frag);
        if (changed) {
            shaderPack = std::move(nextPack);
            vert = std::move(nextVert);
            frag = std::move(nextFrag);
        }
        return changed;
    };

    auto resolveEditorAssetPath = [&](const std::string& rawPath) -> fs::path {
        return resolveProjectAssetPath(rawPath);
    };

    auto revealAssetInProject = [&](const std::string& rawPath) {
        const fs::path resolved = resolveEditorAssetPath(rawPath);
        if (resolved.empty()) {
            return;
        }
        std::error_code ec;
        if (fs::exists(resolved, ec) && !ec) {
            fileBrowser.navigateTo(resolved.parent_path());
            fileBrowser.selectedFile = resolved;
        }
    };

    auto assetDisplayName = [&](const std::string& rawPath, const char* emptyLabel) {
        if (rawPath.empty()) {
            return std::string(emptyLabel);
        }

        const fs::path resolved = resolveEditorAssetPath(rawPath);
        const std::string stem = resolved.stem().string();
        if (!stem.empty()) {
            return stem;
        }

        const std::string fileName = resolved.filename().string();
        if (!fileName.empty()) {
            return fileName;
        }

        return std::string(emptyLabel);
    };

    auto assetHintLabel = [&](const std::string& rawPath, const char* emptyLabel) {
        if (rawPath.empty()) {
            return std::string(emptyLabel);
        }

        const fs::path resolved = resolveEditorAssetPath(rawPath);
        const std::string parentName = resolved.parent_path().filename().string();
        return parentName.empty() ? std::string("Assigned asset") : parentName;
    };

    auto shaderDisplayName = [&](const std::string& shaderPackPath,
                                 const std::string& vertShaderPath,
                                 const std::string& fragShaderPath,
                                 bool preferDefaultEngineShader = true) {
        if (!shaderPackPath.empty()) {
            return assetDisplayName(shaderPackPath, "Shader Pack");
        }

        const MaterialShaderPreset preset = shaderPresetFromPaths(vertShaderPath, fragShaderPath);
        if (preset == MaterialShaderPreset::StandardUnlit) {
            return std::string("Standard Unlit");
        }
        if (preset == MaterialShaderPreset::ScrollingUV) {
            return std::string("Scrolling UV");
        }
        if (preset == MaterialShaderPreset::ProceduralClouds) {
            return std::string("Procedural Clouds");
        }
        if (preset == MaterialShaderPreset::EngineLit && preferDefaultEngineShader) {
            return std::string("Engine Lit");
        }

        const std::string shaderPath = !fragShaderPath.empty() ? fragShaderPath : vertShaderPath;
        if (!shaderPath.empty()) {
            return assetDisplayName(shaderPath, "Legacy Custom Shader");
        }

        return preferDefaultEngineShader ? std::string("Engine Lit") : std::string("Default");
    };

    auto drawInspectorIconButton = [&](const char* id,
                                       const InspectorUiIcon& icon,
                                       const char* fallbackLabel,
                                       const char* tooltip,
                                       bool flipYOverride = false) {
        bool clicked = false;
        const float buttonSize = std::max(18.0f, ImGui::GetFrameHeight() - 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
        if (icon.id != static_cast<ImTextureID>(0)) {
            const bool flipY = icon.flipY || flipYOverride;
            const ImVec2 uvMin = flipY ? ImVec2(0, 1) : ImVec2(0, 0);
            const ImVec2 uvMax = flipY ? ImVec2(1, 0) : ImVec2(1, 1);
            clicked = ImGui::ImageButton(id, icon.id, ImVec2(buttonSize, buttonSize), uvMin, uvMax);
        } else {
            clicked = ImGui::SmallButton(fallbackLabel);
        }
        ImGui::PopStyleVar();
        if (tooltip && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
        return clicked;
    };

    auto drawTransparencyAwareBackground = [](ImDrawList* drawList,
                                              const ImVec2& minPos,
                                              const ImVec2& maxPos,
                                              float rounding,
                                              float cellSize = 10.0f) {
        DrawTransparencyCheckerboard(drawList, minPos, maxPos, rounding, cellSize);
    };

    auto matchesPopupFilter = [](const std::string& text, const std::string& filter) {
        return MatchesAssetFilter(text, filter);
    };

    auto collectAssetsInDirectory = [](const fs::path& root,
                                       const std::function<bool(const fs::directory_entry&)>& predicate) {
        return CollectAssetsInDirectory(root, predicate);
    };

    auto collectProjectTextureAssets = [&]() {
        fs::path root = projectManager.currentProject.isLoaded
            ? projectManager.currentProject.assetsPath
            : fileBrowser.projectRoot;
        return collectAssetsInDirectory(root, [&](const fs::directory_entry& entry) {
            return fileBrowser.isTextureFile(entry);
        });
    };

    auto collectProjectShaderPackAssets = [&]() {
        std::vector<fs::path> matches;
        std::vector<fs::path> roots;
        roots.push_back(projectManager.currentProject.isLoaded
            ? projectManager.currentProject.assetsPath
            : fileBrowser.projectRoot);
        roots.push_back(fs::path("Resources") / "ThirdParty");

        for (const fs::path& root : roots) {
            std::vector<fs::path> rootMatches = collectAssetsInDirectory(root, [&](const fs::directory_entry& entry) {
                return IsMaterialShaderAssetFile(entry.path());
            });
            matches.insert(matches.end(), rootMatches.begin(), rootMatches.end());
        }

        std::sort(matches.begin(), matches.end(), [](const fs::path& a, const fs::path& b) {
            return a.string() < b.string();
        });
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
        return matches;
    };

    auto isTextureOrSpriteSheetSelection = [&](const fs::path& path) {
        if (path.empty() || !fs::exists(path)) return false;
        std::error_code ec;
        fs::directory_entry entry(path, ec);
        if (ec) return false;
        return fileBrowser.isTextureFile(entry) || IsSpriteSheetSidecarPath(path);
    };
    auto assignSpriteTextureOrClips = [&](SceneObject& target, const fs::path& sourcePath) -> bool {
        if (sourcePath.empty() || !fs::exists(sourcePath)) {
            return false;
        }

        const fs::path imagePath = ResolveSpriteSheetImagePath(sourcePath);
        if (!fs::exists(imagePath)) {
            return false;
        }

        target.albedoTexturePath = imagePath.string();
        const fs::path sidecarPath = IsSpriteSheetSidecarPath(sourcePath) ? sourcePath : fs::path(imagePath.string() + ".spritesheet");
        std::vector<glm::ivec4> clips;
        std::vector<std::string> clipNames;
        std::vector<glm::vec2> clipScales;
        // I swear, trying to get this working with the spritesheet management system took far too long lmfao, at least I got it down.
        if (fs::exists(sidecarPath)) {
            if (std::optional<SpritesheetDocument> sidecar = LoadSpriteSheetDocument(sidecarPath)) {
                clips = std::move(sidecar->rects);
                clipNames = std::move(sidecar->names);
                clipScales = std::move(sidecar->scales);
            }
        }

        target.ui.spriteCustomFrames = std::move(clips);
        target.ui.spriteCustomFrameNames = std::move(clipNames);
        target.ui.spriteCustomFrameScales = std::move(clipScales);
        EnsureSpriteClipNames(target.ui.spriteCustomFrameNames, target.ui.spriteCustomFrames.size());
        EnsureSpriteClipScales(target.ui.spriteCustomFrameScales, target.ui.spriteCustomFrames.size());
        target.ui.spriteCustomFramesEnabled = !target.ui.spriteCustomFrames.empty();
        target.ui.spriteSheetEnabled = target.ui.spriteCustomFramesEnabled || target.ui.spriteSheetEnabled;
        target.ui.spriteSheetFrame = 0;
        target.ui.spriteSourceWidth = 0;
        target.ui.spriteSourceHeight = 0;

        if (Texture* tex = renderer.getTexture(target.albedoTexturePath, MaterialProperties::TextureFilter::Point)) {
            target.ui.spriteSourceWidth = tex->GetWidth();
            target.ui.spriteSourceHeight = tex->GetHeight();
        }

        if (target.ui.spriteCustomFramesEnabled) {
            // GOD damn, this sprite used to hate scaling properly to the object. BRUH.
            target.ui.size.x = static_cast<float>(std::max(1, target.ui.spriteCustomFrames[0].z));
            target.ui.size.y = static_cast<float>(std::max(1, target.ui.spriteCustomFrames[0].w));
        }
        return true;
    };

    auto drawMaterialTextureField = [&](const char* label,
                                        const char* idSuffix,
                                        std::string& path) {
        static std::unordered_map<std::string, std::string> popupFilters;

        bool changed = false;
        ImGui::PushID(idSuffix);

        const float rowHeight = std::max(20.0f, ImGui::GetFrameHeight());
        const float thumbSize = rowHeight;
        const float compactButtonSize = rowHeight;
        const float itemSpacingX = ImGui::GetStyle().ItemSpacing.x;
        const std::string popupId = std::string("##TexturePicker_") + idSuffix;
        const std::string displayName = assetDisplayName(path, "None");
        const fs::path resolvedPath = resolveEditorAssetPath(path);

        drawMaterialInlineLabel(label);
        ImGui::SameLine();

        Texture* thumbTexture = nullptr;
        if (!resolvedPath.empty()) {
            thumbTexture = renderer.getTexture(resolvedPath.string(), MaterialProperties::TextureFilter::Bilinear);
        }

        const ImVec2 thumbScreenPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##Thumb", ImVec2(thumbSize, thumbSize));
        ImDrawList* thumbDrawList = ImGui::GetWindowDrawList();
        const ImVec2 thumbMin = thumbScreenPos;
        const ImVec2 thumbMax = ImVec2(thumbScreenPos.x + thumbSize, thumbScreenPos.y + thumbSize);
        drawTransparencyAwareBackground(thumbDrawList, thumbMin, thumbMax, 4.0f, 6.0f);
        if (thumbTexture && thumbTexture->GetID()) {
            const float inset = 1.0f;
            thumbDrawList->AddImage(
                (ImTextureID)(intptr_t)thumbTexture->GetID(),
                ImVec2(thumbMin.x + inset, thumbMin.y + inset),
                ImVec2(thumbMax.x - inset, thumbMax.y - inset),
                ImVec2(0, 1),
                ImVec2(1, 0)
            );
        } else {
            const ImVec2 center((thumbMin.x + thumbMax.x) * 0.5f, (thumbMin.y + thumbMax.y) * 0.5f);
            thumbDrawList->AddLine(ImVec2(center.x - 5.0f, center.y), ImVec2(center.x + 5.0f, center.y),
                                   IM_COL32(184, 190, 204, 180), 1.5f);
            thumbDrawList->AddLine(ImVec2(center.x, center.y - 5.0f), ImVec2(center.x, center.y + 5.0f),
                                   IM_COL32(184, 190, 204, 180), 1.5f);
        }
        if (!path.empty() && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            revealAssetInProject(path);
        }
        if (!path.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reveal in Project\n%s", path.c_str());
        }

        ImGui::SameLine();
        const float controlsWidth = compactButtonSize * 2.0f + itemSpacingX * 2.0f;
        const float fieldWidth = std::max(40.0f, ImGui::GetContentRegionAvail().x - controlsWidth);
        const ImVec2 fieldMin = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##TextureField", ImVec2(fieldWidth, rowHeight))) {
            ImGui::OpenPopup(popupId.c_str());
        }
        const ImVec2 fieldMax(fieldMin.x + fieldWidth, fieldMin.y + rowHeight);
        const std::string fieldText = fitLabelToWidth(path.empty() ? std::string("Empty") : displayName, fieldWidth - 14.0f);
        ImDrawList* fieldDrawList = ImGui::GetWindowDrawList();
        fieldDrawList->PushClipRect(ImVec2(fieldMin.x + 6.0f, fieldMin.y),
                                    ImVec2(fieldMax.x - 6.0f, fieldMax.y),
                                    true);
        fieldDrawList->AddText(
            ImVec2(fieldMin.x + 6.0f, fieldMin.y + std::max(0.0f, (rowHeight - ImGui::GetTextLineHeight()) * 0.5f)),
            ImGui::GetColorU32(ImGuiCol_Text),
            fieldText.c_str()
        );
        fieldDrawList->PopClipRect();
        if (ImGui::IsItemHovered()) {
            if (path.empty()) {
                ImGui::SetTooltip("%s\nDrag an image here or choose one from the asset list.",
                                  assetHintLabel(path, "No texture assigned").c_str());
            } else {
                ImGui::SetTooltip("%s", path.c_str());
            }
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
                const char* dropped = static_cast<const char*>(payload->Data);
                std::error_code ec;
                fs::directory_entry droppedEntry(fs::path(dropped), ec);
                const fs::path droppedPath(dropped);
                if ((!ec && fileBrowser.isTextureFile(droppedEntry)) || IsSpriteSheetSidecarPath(droppedPath)) {
                    path = ResolveSpriteSheetImagePath(droppedPath).string();
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        if (drawInspectorIconButton("##PickAsset", iconTextureSelect, "...", "Choose texture asset")) {
            ImGui::OpenPopup(popupId.c_str());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(path.empty());
        if (ImGui::Button("X", ImVec2(compactButtonSize, rowHeight))) {
            path.clear();
            changed = true;
        }
        ImGui::EndDisabled();

        if (ImGui::BeginPopup(popupId.c_str())) {
            std::string& filter = popupFilters[popupId];
            char searchBuf[128] = {};
            std::snprintf(searchBuf, sizeof(searchBuf), "%s", filter.c_str());
            if (ImGui::InputTextWithHint("##Filter", "Filter textures", searchBuf, sizeof(searchBuf))) {
                filter = searchBuf;
            }

            const bool canUseSelected = isTextureOrSpriteSheetSelection(fileBrowser.selectedFile);
            if (canUseSelected && ImGui::Selectable("Use Selected Asset")) {
                path = ResolveSpriteSheetImagePath(fileBrowser.selectedFile).string();
                changed = true;
                ImGui::CloseCurrentPopup();
            }

            ImGui::Separator();
            ImGui::BeginChild("##TextureList", ImVec2(320.0f, 180.0f), false);
            for (const fs::path& assetPath : collectProjectTextureAssets()) {
                const std::string assetName = assetDisplayName(assetPath.string(), "Texture");
                const std::string assetInfo = assetHintLabel(assetPath.string(), "Texture");
                if (!matchesPopupFilter(assetName + " " + assetInfo, filter)) {
                    continue;
                }

                const bool selected = resolveEditorAssetPath(path) == assetPath;
                if (ImGui::Selectable((assetName + "##" + assetPath.string()).c_str(), selected)) {
                    path = assetPath.string();
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", assetPath.string().c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }

        ImGui::PopID();
        return changed;
    };

    auto drawMaterialShaderPackField = [&](const char* label,
                                           const char* idSuffix,
                                           std::string& shaderPackPath,
                                           std::string& vertShaderPath,
                                           std::string& fragShaderPath,
                                           const MaterialProperties& material,
                                           const std::string& albedoPath,
                                           const std::string& overlayPath,
                                           const std::string& normalPath,
                                           bool useOverlay,
                                           int previewSlot) {
        static std::unordered_map<std::string, std::string> popupFilters;
        (void)material;
        (void)albedoPath;
        (void)overlayPath;
        (void)normalPath;
        (void)useOverlay;
        (void)previewSlot;

        bool changed = false;
        ImGui::PushID(idSuffix);
        const std::string popupId = std::string("##ShaderPackPicker_") + idSuffix;
        const float rowHeight = std::max(20.0f, ImGui::GetFrameHeight());
        const float compactButtonSize = rowHeight;
        const float itemSpacingX = ImGui::GetStyle().ItemSpacing.x;

        auto assignShaderPack = [&](const std::string& packPath) {
            ShaderPackAssetData packData;
            if (!ReadShaderPackFile(packPath, packData)) {
                return false;
            }
            shaderPackPath = packPath;
            vertShaderPath = packData.vertexShaderPath;
            fragShaderPath = packData.fragmentShaderPath;
            return true;
        };

        std::string selectionLabel = "Engine Lit";
        std::string selectionHint = "Default engine shader";
        if (!shaderPackPath.empty()) {
            selectionLabel = assetDisplayName(shaderPackPath, "Shader Pack");
            selectionHint = "Shader Pack";
        } else {
            MaterialShaderPreset preset = shaderPresetFromPaths(vertShaderPath, fragShaderPath);
            if (preset == MaterialShaderPreset::StandardUnlit) {
                selectionLabel = "Standard Unlit";
                selectionHint = "Default engine shader";
            } else if (preset == MaterialShaderPreset::ScrollingUV) {
                selectionLabel = "Scrolling UV";
                selectionHint = "Default engine shader";
            } else if (preset == MaterialShaderPreset::ProceduralClouds) {
                selectionLabel = "Procedural Clouds";
                selectionHint = "Default engine shader";
            } else if (preset == MaterialShaderPreset::Custom && (!vertShaderPath.empty() || !fragShaderPath.empty())) {
                selectionLabel = "Legacy Custom Shader";
                selectionHint = "Using internal shader paths";
            }
        }

        drawMaterialInlineLabel(label);
        ImGui::SameLine();

        const float controlsWidth = compactButtonSize * 2.0f + itemSpacingX * 2.0f;
        const float fieldWidth = std::max(140.0f, ImGui::GetContentRegionAvail().x - controlsWidth);
        const ImVec2 fieldMin = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##ShaderPack", ImVec2(fieldWidth, rowHeight))) {
            ImGui::OpenPopup(popupId.c_str());
        }
        const ImVec2 fieldMax(fieldMin.x + fieldWidth, fieldMin.y + rowHeight);
        ImDrawList* fieldDrawList = ImGui::GetWindowDrawList();
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
        const std::string fieldText = fitLabelToWidth(selectionLabel, fieldWidth - 14.0f);
        const ImVec2 textMin(fieldMin.x + 6.0f, fieldMin.y);
        const ImVec2 textMax(fieldMax.x - 8.0f, fieldMax.y);
        fieldDrawList->PushClipRect(textMin, textMax, true);
        fieldDrawList->AddText(
            ImVec2(textMin.x, fieldMin.y + std::max(0.0f, (rowHeight - ImGui::GetTextLineHeight()) * 0.5f)),
            textColor,
            fieldText.c_str()
        );
        fieldDrawList->PopClipRect();
        if (ImGui::IsItemHovered()) {
            if (!shaderPackPath.empty()) {
                ImGui::SetTooltip("%s", shaderPackPath.c_str());
            } else if (!vertShaderPath.empty() || !fragShaderPath.empty()) {
                ImGui::SetTooltip("%s\nVertex: %s\nFragment: %s", selectionHint.c_str(),
                                  vertShaderPath.c_str(), fragShaderPath.c_str());
            } else {
                ImGui::SetTooltip("%s", selectionHint.c_str());
            }
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
                const char* dropped = static_cast<const char*>(payload->Data);
                const fs::path droppedPath(dropped ? dropped : "");
                if (IsMaterialShaderAssetFile(droppedPath) && assignShaderPack(droppedPath.string())) {
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (drawInspectorIconButton("##PickShaderPack", iconTextureSelect, "...", "Choose shader pack")) {
            ImGui::OpenPopup(popupId.c_str());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(shaderPackPath.empty() && vertShaderPath.empty() && fragShaderPath.empty());
        if (ImGui::Button("X", ImVec2(compactButtonSize, rowHeight))) {
            shaderPackPath.clear();
            vertShaderPath.clear();
            fragShaderPath.clear();
            changed = true;
        }
        ImGui::EndDisabled();

        if (ImGui::BeginPopup(popupId.c_str())) {
            std::string& filter = popupFilters[popupId];
            char searchBuf[128] = {};
            std::snprintf(searchBuf, sizeof(searchBuf), "%s", filter.c_str());
            if (ImGui::InputTextWithHint("##Filter", "Filter shaders", searchBuf, sizeof(searchBuf))) {
                filter = searchBuf;
            }

            if (matchesPopupFilter("Engine Lit", filter) &&
                ImGui::Selectable("Engine Lit (Default)", shaderPresetFromPaths(vertShaderPath, fragShaderPath) == MaterialShaderPreset::EngineLit &&
                                                          shaderPackPath.empty())) {
                changed |= applyShaderPreset(MaterialShaderPreset::EngineLit, shaderPackPath, vertShaderPath, fragShaderPath);
                ImGui::CloseCurrentPopup();
            }
            if (matchesPopupFilter("Standard Unlit", filter) &&
                ImGui::Selectable("Standard Unlit", shaderPresetFromPaths(vertShaderPath, fragShaderPath) == MaterialShaderPreset::StandardUnlit &&
                                                   shaderPackPath.empty())) {
                changed |= applyShaderPreset(MaterialShaderPreset::StandardUnlit, shaderPackPath, vertShaderPath, fragShaderPath);
                ImGui::CloseCurrentPopup();
            }
            if (matchesPopupFilter("Scrolling UV", filter) &&
                ImGui::Selectable("Scrolling UV", shaderPresetFromPaths(vertShaderPath, fragShaderPath) == MaterialShaderPreset::ScrollingUV &&
                                                shaderPackPath.empty())) {
                changed |= applyShaderPreset(MaterialShaderPreset::ScrollingUV, shaderPackPath, vertShaderPath, fragShaderPath);
                ImGui::CloseCurrentPopup();
            }
            if (matchesPopupFilter("Procedural Clouds", filter) &&
                ImGui::Selectable("Procedural Clouds", shaderPresetFromPaths(vertShaderPath, fragShaderPath) == MaterialShaderPreset::ProceduralClouds &&
                                                      shaderPackPath.empty())) {
                changed |= applyShaderPreset(MaterialShaderPreset::ProceduralClouds, shaderPackPath, vertShaderPath, fragShaderPath);
                ImGui::CloseCurrentPopup();
            }

            const bool selectedIsPack = IsMaterialShaderAssetFile(fileBrowser.selectedFile);
            if (selectedIsPack) {
                ImGui::Separator();
                if (ImGui::Selectable("Use Selected Shader")) {
                    if (assignShaderPack(fileBrowser.selectedFile.string())) {
                        changed = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }

            ImGui::Separator();
            ImGui::BeginChild("##ShaderPackList", ImVec2(320.0f, 180.0f), false);
            for (const fs::path& assetPath : collectProjectShaderPackAssets()) {
                const std::string assetName = assetDisplayName(assetPath.string(), "Shader");
                const std::string assetInfo = assetHintLabel(assetPath.string(), "Shader");
                if (!matchesPopupFilter(assetName + " " + assetInfo, filter)) {
                    continue;
                }

                ShaderPackAssetData packData;
                if (!ReadShaderPackFile(assetPath.string(), packData)) {
                    ImGui::BeginDisabled();
                    ImGui::Selectable((assetName + " (invalid)##" + assetPath.string()).c_str(), false);
                    ImGui::EndDisabled();
                    continue;
                }

                const bool selected = resolveEditorAssetPath(shaderPackPath) == assetPath;
                if (ImGui::Selectable((assetName + "##" + assetPath.string()).c_str(), selected)) {
                    if (assignShaderPack(assetPath.string())) {
                        changed = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", assetPath.string().c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }

        ImGui::PopID();
        return changed;
    };
    (void)drawMaterialShaderPackField;

    auto drawMaterialShaderSelectorInline = [&](const char* idSuffix,
                                                std::string& shaderPackPath,
                                                std::string& vertShaderPath,
                                                std::string& fragShaderPath,
                                                float width,
                                                float height) {
        static std::unordered_map<std::string, std::string> popupFilters;

        bool changed = false;
        ImGui::PushID(idSuffix);
        const std::string popupId = std::string("##HeaderShaderPackPicker_") + idSuffix;

        auto assignShaderPack = [&](const std::string& packPath) {
            ShaderPackAssetData packData;
            if (!ReadShaderPackFile(packPath, packData)) {
                return false;
            }
            shaderPackPath = packPath;
            vertShaderPath = packData.vertexShaderPath;
            fragShaderPath = packData.fragmentShaderPath;
            return true;
        };

        const std::string selectionLabel = shaderDisplayName(shaderPackPath, vertShaderPath, fragShaderPath, false);
        const float arrowRegionWidth = std::min(18.0f, std::max(12.0f, height - 2.0f));
        const std::string clippedLabel = fitLabelToWidth(selectionLabel, width - arrowRegionWidth - 14.0f);
        const ImVec2 fieldMin = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##HeaderShaderSelector", ImVec2(width, height))) {
            ImGui::OpenPopup(popupId.c_str());
        }
        const ImVec2 fieldMax(fieldMin.x + width, fieldMin.y + height);
        ImDrawList* fieldDrawList = ImGui::GetWindowDrawList();
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const ImU32 frameColor = ImGui::GetColorU32(
            held ? ImGuiCol_FrameBgActive : (hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg));
        const ImU32 borderColor = ImGui::GetColorU32(ImGuiCol_Border);
        fieldDrawList->AddRectFilled(fieldMin, fieldMax, frameColor, 4.0f);
        fieldDrawList->AddRect(fieldMin, fieldMax, borderColor, 4.0f);
        fieldDrawList->PushClipRect(ImVec2(fieldMin.x + 6.0f, fieldMin.y),
                                    ImVec2(fieldMax.x - arrowRegionWidth - 6.0f, fieldMax.y),
                                    true);
        fieldDrawList->AddText(
            ImVec2(fieldMin.x + 6.0f, fieldMin.y + std::max(0.0f, (height - ImGui::GetTextLineHeight()) * 0.5f)),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            clippedLabel.c_str()
        );
        fieldDrawList->PopClipRect();
        const float arrowCenterX = fieldMax.x - arrowRegionWidth * 0.5f - 2.0f;
        const float arrowCenterY = fieldMin.y + height * 0.5f + 1.0f;
        fieldDrawList->AddTriangleFilled(
            ImVec2(arrowCenterX - 4.0f, arrowCenterY - 2.0f),
            ImVec2(arrowCenterX + 4.0f, arrowCenterY - 2.0f),
            ImVec2(arrowCenterX, arrowCenterY + 2.5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled)
        );
        if (ImGui::IsItemHovered()) {
            if (!shaderPackPath.empty()) {
                ImGui::SetTooltip("%s", shaderPackPath.c_str());
            } else if (!vertShaderPath.empty() || !fragShaderPath.empty()) {
                ImGui::SetTooltip("Vertex: %s\nFragment: %s", vertShaderPath.c_str(), fragShaderPath.c_str());
            } else {
                ImGui::SetTooltip("Choose shader");
            }
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
                const char* dropped = static_cast<const char*>(payload->Data);
                const fs::path droppedPath(dropped ? dropped : "");
                if (IsMaterialShaderAssetFile(droppedPath) && assignShaderPack(droppedPath.string())) {
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopup(popupId.c_str())) {
            std::string& filter = popupFilters[popupId];
            char searchBuf[128] = {};
            std::snprintf(searchBuf, sizeof(searchBuf), "%s", filter.c_str());
            if (ImGui::InputTextWithHint("##Filter", "Filter shaders", searchBuf, sizeof(searchBuf))) {
                filter = searchBuf;
            }

            if (matchesPopupFilter("Engine Lit", filter) &&
                ImGui::Selectable("Engine Lit (Default)",
                                  shaderPresetFromPaths(vertShaderPath, fragShaderPath) == MaterialShaderPreset::EngineLit &&
                                  shaderPackPath.empty())) {
                changed |= applyShaderPreset(MaterialShaderPreset::EngineLit, shaderPackPath, vertShaderPath, fragShaderPath);
                ImGui::CloseCurrentPopup();
            }
            if (matchesPopupFilter("Standard Unlit", filter) &&
                ImGui::Selectable("Standard Unlit",
                                  shaderPresetFromPaths(vertShaderPath, fragShaderPath) == MaterialShaderPreset::StandardUnlit &&
                                  shaderPackPath.empty())) {
                changed |= applyShaderPreset(MaterialShaderPreset::StandardUnlit, shaderPackPath, vertShaderPath, fragShaderPath);
                ImGui::CloseCurrentPopup();
            }
            if (matchesPopupFilter("Scrolling UV", filter) &&
                ImGui::Selectable("Scrolling UV",
                                  shaderPresetFromPaths(vertShaderPath, fragShaderPath) == MaterialShaderPreset::ScrollingUV &&
                                  shaderPackPath.empty())) {
                changed |= applyShaderPreset(MaterialShaderPreset::ScrollingUV, shaderPackPath, vertShaderPath, fragShaderPath);
                ImGui::CloseCurrentPopup();
            }
            if (matchesPopupFilter("Procedural Clouds", filter) &&
                ImGui::Selectable("Procedural Clouds",
                                  shaderPresetFromPaths(vertShaderPath, fragShaderPath) == MaterialShaderPreset::ProceduralClouds &&
                                  shaderPackPath.empty())) {
                changed |= applyShaderPreset(MaterialShaderPreset::ProceduralClouds, shaderPackPath, vertShaderPath, fragShaderPath);
                ImGui::CloseCurrentPopup();
            }

            const bool selectedIsPack = IsMaterialShaderAssetFile(fileBrowser.selectedFile);
            if (selectedIsPack) {
                ImGui::Separator();
                if (ImGui::Selectable("Use Selected Shader")) {
                    if (assignShaderPack(fileBrowser.selectedFile.string())) {
                        changed = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }

            ImGui::Separator();
            ImGui::BeginChild("##HeaderShaderPackList", ImVec2(320.0f, 180.0f), false);
            for (const fs::path& assetPath : collectProjectShaderPackAssets()) {
                const std::string assetName = assetDisplayName(assetPath.string(), "Shader");
                const std::string assetInfo = assetHintLabel(assetPath.string(), "Shader");
                if (!matchesPopupFilter(assetName + " " + assetInfo, filter)) {
                    continue;
                }

                ShaderPackAssetData packData;
                if (!ReadShaderPackFile(assetPath.string(), packData)) {
                    ImGui::BeginDisabled();
                    ImGui::Selectable((assetName + " (invalid)##" + assetPath.string()).c_str(), false);
                    ImGui::EndDisabled();
                    continue;
                }

                const bool selected = resolveEditorAssetPath(shaderPackPath) == assetPath;
                if (ImGui::Selectable((assetName + "##" + assetPath.string()).c_str(), selected)) {
                    if (assignShaderPack(assetPath.string())) {
                        changed = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", assetPath.string().c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }

        ImGui::PopID();
        return changed;
    };

    auto drawMaterialSectionHeader = [&](const char* idSuffix,
                                         const std::string& materialName,
                                         std::string& shaderPackPath,
                                         std::string& vertShaderPath,
                                         std::string& fragShaderPath,
                                         const MaterialProperties& material,
                                         const std::string& albedoPath,
                                         const std::string& overlayPath,
                                         const std::string& normalPath,
                                         bool useOverlay,
                                         bool shaderEditable,
                                         int previewSlot,
                                         const std::function<void()>& headerMenu = {}) {
        ImGui::PushID(idSuffix);
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::SetNextItemAllowOverlap();

        // Match the flat component-header styling: subtle separator + transparent header bg.
        {
            ImDrawList* sepDl = ImGui::GetWindowDrawList();
            const ImVec2 sepP = ImGui::GetCursorScreenPos();
            const float sepW = ImGui::GetContentRegionAvail().x;
            const ImU32 sepCol = ImGui::GetColorU32(ImGuiCol_Separator, 0.35f);
            sepDl->AddLine(ImVec2(sepP.x, sepP.y), ImVec2(sepP.x + sepW, sepP.y), sepCol, 1.0f);
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(style.FramePadding.x, std::max(style.FramePadding.y, 11.0f)));
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        const bool open = ImGui::CollapsingHeader("##MaterialHeader",
                                                  ImGuiTreeNodeFlags_DefaultOpen |
                                                  ImGuiTreeNodeFlags_AllowOverlap |
                                                  ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        // Right-click on the header: the rarely needed file actions, kept out of the body.
        if (headerMenu && ImGui::BeginPopupContextItem("##MaterialHeaderMenu")) {
            headerMenu();
            ImGui::EndPopup();
        }

        const ImVec2 headerMin = ImGui::GetItemRectMin();
        const ImVec2 headerMax = ImGui::GetItemRectMax();
        const float headerHeight = headerMax.y - headerMin.y;
        const float previewSize = std::clamp(headerHeight - 10.0f, 20.0f, 30.0f);
        const float arrowWidth = headerHeight;
        const float previewX = headerMin.x + arrowWidth + 2.0f;
        const float previewY = headerMin.y + (headerHeight - previewSize) * 0.5f;
        const float textX = previewX + previewSize + 8.0f;
        const float textRight = headerMax.x - style.FramePadding.x - 6.0f;
        const float textWidth = std::max(24.0f, textRight - textX);
        const std::string primaryText = fitLabelToWidth(materialName.empty() ? std::string("Material") : materialName,
                                                        textWidth);
        const float lineHeight = ImGui::GetTextLineHeight();
        const float lineSpacing = 1.0f;
        const float shaderFieldHeight = std::max(16.0f, lineHeight - 1.0f);
        const float textBlockHeight = lineHeight + lineSpacing + shaderFieldHeight;
        const float textTop = headerMin.y + std::max(0.0f, (headerHeight - textBlockHeight) * 0.5f);
        const float shaderFieldY = textTop + lineHeight + lineSpacing;

        const unsigned int headerPreviewTexture = renderMaterialPreviewTexture(
            material,
            albedoPath,
            overlayPath,
            normalPath,
            useOverlay,
            vertShaderPath,
            fragShaderPath,
            64,
            64,
            previewSlot,
            glm::vec2(12.0f, -8.0f)
        );

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        if (headerPreviewTexture != 0) {
            drawList->PushClipRect(ImVec2(previewX, previewY),
                                   ImVec2(previewX + previewSize, previewY + previewSize),
                                   true);
            drawList->AddImage(
                (ImTextureID)(intptr_t)headerPreviewTexture,
                ImVec2(previewX, previewY),
                ImVec2(previewX + previewSize, previewY + previewSize),
                ImVec2(0, 1),
                ImVec2(1, 0)
            );
            drawList->PopClipRect();
        }

        drawList->PushClipRect(ImVec2(textX, textTop),
                               ImVec2(textRight, textTop + lineHeight),
                               true);
        drawList->AddText(ImVec2(textX, textTop),
                          ImGui::GetColorU32(ImGuiCol_Text),
                          primaryText.c_str());
        drawList->PopClipRect();

        bool changed = false;
        ImGui::SetCursorScreenPos(ImVec2(textX, shaderFieldY));
        if (shaderEditable) {
            changed |= drawMaterialShaderSelectorInline("HeaderShaderSelector",
                                                        shaderPackPath,
                                                        vertShaderPath,
                                                        fragShaderPath,
                                                        textWidth,
                                                        shaderFieldHeight);
        } else {
            ImGui::BeginDisabled();
            ImGui::Button(shaderDisplayName(shaderPackPath, vertShaderPath, fragShaderPath, false).c_str(),
                          ImVec2(textWidth, shaderFieldHeight));
            ImGui::EndDisabled();
        }

        ImGui::PopID();
        return std::pair<bool, bool>{open, changed};
    };

    struct InspectorReveal {
        Engine* engine = nullptr;
        UIAnimationState* anim = nullptr;
        bool rendering = false;
        bool grouped = false;
        bool animating = false;
        ImVec2 layoutCursor = ImVec2(0.0f, 0.0f);
        float reserved = 0.0f;
        float cursorMaxYBefore = 0.0f;

        InspectorReveal() = default;

        InspectorReveal(Engine& eng, ImGuiID key, bool open) {
            engine = &eng;
            if (key == 0) {
                rendering = open;
                return;
            }
            anim = &eng.editorUiAnimationStates[key];

            float speed = 0.0f;
            if (eng.uiAnimationMode == UIAnimationMode::Fluid) speed = 11.0f;
            else if (eng.uiAnimationMode == UIAnimationMode::Snappy) speed = 20.0f;
            const float step = (eng.uiAnimationMode == UIAnimationMode::Off)
                ? 1.0f
                : (1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime));
            const float target = open ? 1.0f : 0.0f;
            if (!anim->initialized) {
                anim->foldOpen = target;
                anim->initialized = true;
            } else {
                anim->foldOpen += (target - anim->foldOpen) * step;
            }
            if (std::abs(anim->foldOpen - target) < 0.002f) anim->foldOpen = target;

            const float t = std::clamp(anim->foldOpen, 0.0f, 1.0f);
            rendering = open || t > 0.002f;
            if (!rendering) return;

            animating = (t < 0.999f) && (anim->contentExtent > 0.0f);
            if (animating) {
                const float eased = t * t * (3.0f - 2.0f * t);
                const float alphaT = std::clamp((t - 0.12f) / 0.88f, 0.0f, 1.0f);
                const float easedAlpha = alphaT * alphaT * (3.0f - 2.0f * alphaT);

                layoutCursor = ImGui::GetCursorPos();
                const ImVec2 screen = ImGui::GetCursorScreenPos();
                ImGuiWindow* window = ImGui::GetCurrentWindow();
                cursorMaxYBefore = window->DC.CursorMaxPos.y;
                reserved = std::max(0.0f, anim->contentExtent) * eased;

                ImGui::PushClipRect(ImVec2(window->Pos.x - 8.0f, screen.y),
                                    ImVec2(window->Pos.x + window->Size.x + 8.0f,
                                           screen.y + reserved),
                                    true);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                    ImGui::GetStyle().Alpha * std::max(0.02f, easedAlpha));
            }
            ImGui::BeginGroup();
            grouped = true;
        }

        InspectorReveal(InspectorReveal&& other) noexcept { steal(other); }
        InspectorReveal& operator=(InspectorReveal&& other) noexcept {
            if (this != &other) { finish(); steal(other); }
            return *this;
        }
        InspectorReveal(const InspectorReveal&) = delete;
        InspectorReveal& operator=(const InspectorReveal&) = delete;
        ~InspectorReveal() { finish(); }

        explicit operator bool() const { return rendering; }

        void steal(InspectorReveal& other) {
            engine = other.engine; anim = other.anim;
            rendering = other.rendering; grouped = other.grouped;
            animating = other.animating; layoutCursor = other.layoutCursor;
            reserved = other.reserved; cursorMaxYBefore = other.cursorMaxYBefore;
            other.engine = nullptr; other.anim = nullptr;
            other.rendering = false; other.grouped = false; other.animating = false;
        }

        void finish() {
            if (!grouped) { engine = nullptr; return; }
            ImGui::EndGroup();
            const float rendered = ImGui::GetItemRectSize().y;
            if (rendered > 0.0f && anim) {
                if (!animating || anim->contentExtent <= 0.0f ||
                    (engine && engine->uiAnimationMode == UIAnimationMode::Off)) {
                    anim->contentExtent = rendered;
                } else {
                    anim->contentExtent += (rendered - anim->contentExtent) * 0.35f;
                }
            }
            if (animating) {
                ImGui::PopStyleVar();
                ImGui::PopClipRect();
                ImGui::SetCursorPos(layoutCursor);
                ImGui::Dummy(ImVec2(0.0f, reserved));
                ImGuiWindow* window = ImGui::GetCurrentWindow();
                window->DC.CursorMaxPos.y =
                    ImMax(cursorMaxYBefore, ImGui::GetCursorScreenPos().y);
            }
            grouped = false;
            animating = false;
            engine = nullptr;
        }
    };

    auto drawInspectorSubsectionFoldout = [&](const char* label,
                                              bool* open = nullptr,
                                              bool defaultOpen = false,
                                              bool enabled = true) -> InspectorReveal {
        if (!enabled) {
            if (open) *open = false;
            return InspectorReveal();
        }
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (defaultOpen) {
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        std::string nodeId = "##";
        nodeId += label;

        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        const bool isOpen = ImGui::TreeNodeEx(nodeId.c_str(), flags);
        ImGui::PopStyleColor(4);

        if (isOpen) {
            ImGui::TreePop();
        }
        if (open) {
            *open = isOpen;
        }

        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const float rowHeight = rowMax.y - rowMin.y;
        const float fontSize = ImGui::GetFontSize();
        ImGuiStyle& style = ImGui::GetStyle();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const ImGuiID arrowKey = ImGui::GetID((nodeId + "##arrow").c_str());
        UIAnimationState& arrowState = editorUiAnimationStates[arrowKey];
        {
            float speed = 0.0f;
            if (uiAnimationMode == UIAnimationMode::Fluid) speed = 12.0f;
            else if (uiAnimationMode == UIAnimationMode::Snappy) speed = 22.0f;
            const float step = (uiAnimationMode == UIAnimationMode::Off)
                ? 1.0f
                : (1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime));
            const float target = isOpen ? 1.0f : 0.0f;
            if (!arrowState.initialized) {
                arrowState.foldOpen = target;
                arrowState.initialized = true;
            } else {
                arrowState.foldOpen += (target - arrowState.foldOpen) * step;
            }
            if (std::abs(arrowState.foldOpen - target) < 0.001f) arrowState.foldOpen = target;
        }

        if (iconCollapseArrow.id != static_cast<ImTextureID>(0)) {
            const float side = (iconCollapseArrow.nativeW > 0.0f)
                                   ? iconCollapseArrow.nativeW
                                   : std::floor(fontSize);
            const float slotX = rowMin.x + style.FramePadding.x;
            ImVec2 center(slotX + fontSize * 0.5f, rowMin.y + rowHeight * 0.5f);
            const bool atRest = (arrowState.foldOpen <= 0.0f || arrowState.foldOpen >= 1.0f);
            if (atRest) {
                center.x = std::floor(center.x - side * 0.5f) + side * 0.5f;
                center.y = std::floor(center.y - side * 0.5f) + side * 0.5f;
            }
            const float angle = arrowState.foldOpen * 1.57079633f;
            const float half = side * 0.5f;
            const float cosA = std::cos(angle);
            const float sinA = std::sin(angle);
            auto corner = [&](float x, float y) {
                return ImVec2(center.x + x * cosA - y * sinA,
                              center.y + x * sinA + y * cosA);
            };
            ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 0.0f), uv2(1.0f, 1.0f), uv3(0.0f, 1.0f);
            if (iconCollapseArrow.flipY) {
                uv0 = ImVec2(0.0f, 1.0f); uv1 = ImVec2(1.0f, 1.0f);
                uv2 = ImVec2(1.0f, 0.0f); uv3 = ImVec2(0.0f, 0.0f);
            }
            dl->AddImageQuad(iconCollapseArrow.id,
                             corner(-half, -half), corner(half, -half),
                             corner(half, half), corner(-half, half),
                             uv0, uv1, uv2, uv3, IM_COL32(255, 255, 255, 225));
        }

        {
            const float textX = rowMin.x + fontSize + style.FramePadding.x * 2.0f;
            const float textY = rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(std::floor(textX), std::floor(textY)),
                        ImGui::GetColorU32(ImGuiCol_Text), label);
        }

        return InspectorReveal(*this, ImGui::GetID((nodeId + "##body").c_str()), isOpen);
    };

    auto renderMaterialEditorBody = [&](const char* idPrefix,
                                        MaterialProperties& materialValue,
                                        std::string& albedoPath,
                                        std::string& overlayPath,
                                        std::string& normalPath,
                                        std::string& metallicRoughnessPath,
                                        std::string& metallicMapPath,
                                        std::string& roughnessMapPath,
                                        std::string& opacityMapPath,
                                        std::string& emissivePath,
                                        std::string& occlusionPath,
                                        std::string& heightPath,
                                        bool& useOverlayValue,
                                        std::string& shaderPackPath,
                                        std::string& vertShaderPath,
                                        std::string& fragShaderPath,
                                        SceneObject* spriteTarget,
                                        float& previewScale,
                                        int previewSlot) {
        (void)shaderPackPath;
        bool changed = false;
        ImGui::PushID(idPrefix);

        glm::vec4 baseColor(materialValue.color, materialValue.alpha);
        if (materialColorSamplerHasResult && materialColorSamplerTargetId == idPrefix) {
            baseColor.x = materialColorSamplerResult.x;
            baseColor.y = materialColorSamplerResult.y;
            baseColor.z = materialColorSamplerResult.z;
            materialValue.color = glm::vec3(baseColor);
            materialColorSamplerHasResult = false;
            materialColorSamplerTargetId.clear();
            changed = true;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Color");
        ImGui::SameLine(98.0f);
        if (ImGui::ColorButton("##BaseColorSwatch",
                               ImVec4(baseColor.x, baseColor.y, baseColor.z, baseColor.w),
                               ImGuiColorEditFlags_AlphaPreviewHalf,
                               ImVec2(std::max(20.0f, ImGui::GetFrameHeight()), ImGui::GetFrameHeight()))) {
            ImGui::OpenPopup("##BaseColorPicker");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(usingVulkan());
        if (drawInspectorIconButton("##BaseColorSamplerButton", iconColorPicker, "Pick", "Sample base color from screen", true)) {
            materialColorSamplerActive = true;
            materialColorSamplerAwaitMouseRelease = true;
            materialColorSamplerHasResult = false;
            materialColorSamplerTargetId = idPrefix;
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("##BaseColorPicker")) {
            if (ImGui::ColorPicker4("##BaseColorPickerValue", &baseColor.x,
                                    ImGuiColorEditFlags_PickerHueBar |
                                    ImGuiColorEditFlags_DisplayRGB |
                                    ImGuiColorEditFlags_InputRGB |
                                    ImGuiColorEditFlags_AlphaBar |
                                    ImGuiColorEditFlags_AlphaPreviewHalf)) {
                materialValue.color = glm::vec3(baseColor);
                materialValue.alpha = std::clamp(baseColor.w, 0.0f, 1.0f);
                changed = true;
            }
            ImGui::EndPopup();
        }
        if (materialColorSamplerActive && materialColorSamplerTargetId == idPrefix) {
            ImGui::SameLine();
            ImGui::TextDisabled("Sampling...");
        }

        if (ImGui::Checkbox(Loc::Widget("INSPECTOR_PBR_ENABLED", "Physically Based"),
                            &materialValue.pbrEnabled)) {
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Energy-conserving Cook-Torrance shading. Metallic and Smoothness\n"
                              "below then drive real metalness and roughness instead of the\n"
                              "specular strength and Blinn-Phong exponent they stand in for.");
        }

        const bool metallicFromMap = materialValue.pbrEnabled &&
                                     (!metallicMapPath.empty() || !metallicRoughnessPath.empty());
        const bool roughnessFromMap = materialValue.pbrEnabled &&
                                      (!roughnessMapPath.empty() || !metallicRoughnessPath.empty());

        float metallic = materialValue.pbrEnabled ? materialValue.metallic
                                                  : materialValue.specularStrength;
        ImGui::BeginDisabled(metallicFromMap);
        if (ImGui::SliderFloat(Loc::Widget("INSPECTOR_METALLIC", "Metallic"), &metallic, 0.0f, 1.0f)) {
            if (materialValue.pbrEnabled) {
                materialValue.metallic = std::clamp(metallic, 0.0f, 1.0f);
            } else {
                materialValue.specularStrength = metallic;
            }
            changed = true;
        }
        ImGui::EndDisabled();
        if (metallicFromMap && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Driven by the metallic map. Clear it to use this slider.");
        }

        float smoothness = materialValue.pbrEnabled ? (1.0f - materialValue.roughness)
                                                    : (materialValue.shininess / 256.0f);
        ImGui::BeginDisabled(roughnessFromMap);
        if (ImGui::SliderFloat(Loc::Widget("INSPECTOR_SMOOTHNESS", "Smoothness"), &smoothness, 0.0f, 1.0f)) {
            smoothness = std::clamp(smoothness, 0.0f, 1.0f);
            if (materialValue.pbrEnabled) {
                materialValue.roughness = 1.0f - smoothness;
            } else {
                materialValue.shininess = smoothness * 256.0f;
            }
            changed = true;
        }
        ImGui::EndDisabled();
        if (roughnessFromMap && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Driven by the roughness map. Clear it to use this slider.");
        }

        if (ImGui::SliderFloat(Loc::Widget("INSPECTOR_AMBIENT_LIGHT", "Ambient Light"), &materialValue.ambientStrength, 0.0f, 1.0f)) {
            changed = true;
        }

        ImGui::Spacing();
        if (InspectorReveal _es = drawInspectorSubsectionFoldout("Emission", nullptr, false)) {
            drawMaterialInlineLabel("Color");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::ColorEdit3("##EmissionColor", &materialValue.emissionColor.x,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                changed = true;
            }
            drawMaterialInlineLabel("Brightness");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##EmissionStrength", &materialValue.emissionStrength,
                                 0.02f, 0.0f, 64.0f, "%.2f")) {
                materialValue.emissionStrength = std::max(0.0f, materialValue.emissionStrength);
                changed = true;
            }
            if (drawMaterialTextureField("Emissive Map",
                                         (std::string(idPrefix) + "Emissive").c_str(),
                                         emissivePath)) {
                changed = true;
                if (!emissivePath.empty() && materialValue.emissionStrength <= 0.0f) {
                    materialValue.emissionStrength = 1.5f;
                }
            }
            if (!emissivePath.empty() && materialValue.emissionStrength <= 0.0f) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "Map assigned but Brightness is 0 - nothing will glow.");
            }
            if (materialValue.emissionStrength > 0.0f && materialValue.emissionStrength < 1.25f) {
                ImGui::TextDisabled("Above ~1.25 to bloom");
            }
        }

        ImGui::Spacing();
        if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Texture Maps", nullptr, true)) {
            changed |= drawMaterialTextureField("Base Map", (std::string(idPrefix) + "Albedo").c_str(), albedoPath);
            changed |= drawMaterialTextureField("Normal Map", (std::string(idPrefix) + "Normal").c_str(), normalPath);
            // frag.glsl reads these three only on its PBR branch; the legacy branch takes
            // metalness and smoothness from the sliders alone. The height/parallax rows
            // further down are PBR-only too (both fragment shaders gate parallax on
            // usePbr). Occlusion, opacity and normal are read by both branches.
            if (materialValue.pbrEnabled) {
                changed |= drawMaterialTextureField("Metallic", (std::string(idPrefix) + "MetallicMap").c_str(), metallicMapPath);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Single-channel metalness, read from red.\n"
                                      "Replaces the Metallic slider. Wins over Metal/Rough.");
                }
                changed |= drawMaterialTextureField("Roughness", (std::string(idPrefix) + "RoughnessMap").c_str(), roughnessMapPath);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Single-channel roughness, read from red.\n"
                                      "Replaces the Smoothness slider. Wins over Metal/Rough.");
                }
                changed |= drawMaterialTextureField("Metal/Rough", (std::string(idPrefix) + "MetallicRoughness").c_str(), metallicRoughnessPath);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Packed glTF ORM: roughness in green, metallic in blue.\n"
                                      "Use this only for a genuinely packed texture - a\n"
                                      "greyscale metalness file dropped here also overwrites\n"
                                      "roughness. Separate files go in the two slots above.");
                }
            } else {
                // Hidden, not cleared: say so, or an assigned map looks like it vanished.
                const int hiddenPbrMaps = (metallicMapPath.empty() ? 0 : 1) +
                                          (roughnessMapPath.empty() ? 0 : 1) +
                                          (metallicRoughnessPath.empty() ? 0 : 1) +
                                          (heightPath.empty() ? 0 : 1);
                if (hiddenPbrMaps > 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("%d PBR map%s (metallic, roughness or height) kept for Physically Based.",
                                       hiddenPbrMaps, hiddenPbrMaps == 1 ? "" : "s");
                    ImGui::PopStyleColor();
                }
            }
            changed |= drawMaterialTextureField("Opacity", (std::string(idPrefix) + "OpacityMap").c_str(), opacityMapPath);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Alpha mask, read from red. Replaces the base map's\n"
                                  "alpha channel. Pair with Alpha Cutoff below for a\n"
                                  "hard cutout instead of a blend.");
            }
            drawMaterialInlineLabel("Alpha Cutoff");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##AlphaCutoff", &materialValue.alphaCutoff, 0.0f, 1.0f, "%.2f")) {
                materialValue.alphaCutoff = std::clamp(materialValue.alphaCutoff, 0.0f, 1.0f);
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Discard fragments below this alpha outright.\n"
                                  "0 disables the test and leaves the material blended.");
            }
            changed |= drawMaterialTextureField("Occlusion", (std::string(idPrefix) + "Occlusion").c_str(), occlusionPath);
            drawMaterialInlineLabel("Occlusion Strength");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##OcclusionStrength", &materialValue.occlusionStrength, 0.0f, 1.0f, "%.2f")) {
                materialValue.occlusionStrength = std::clamp(materialValue.occlusionStrength, 0.0f, 1.0f);
                changed = true;
            }
            if (materialValue.pbrEnabled) {
                changed |= drawMaterialTextureField("Height Map", (std::string(idPrefix) + "Height").c_str(), heightPath);
                drawMaterialInlineLabel("Height Scale");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##HeightScale", &materialValue.heightScale, 0.0f, 0.2f, "%.3f")) {
                    materialValue.heightScale = std::clamp(materialValue.heightScale, 0.0f, 1.0f);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Parallax occlusion depth, in the material's UV space, so it\n"
                                      "scales with UV Tiling. 0 turns the ray march off entirely.\n"
                                      "Past about 0.1 the silhouette stops selling the illusion.");
                }
                ImGui::BeginDisabled(materialValue.heightScale <= 0.0f);
                drawMaterialInlineLabel("Parallax Steps");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt("##ParallaxSteps", &materialValue.parallaxSteps, 4, 256)) {
                    materialValue.parallaxSteps = std::clamp(materialValue.parallaxSteps, 4, 256);
                    changed = true;
                }
                drawMaterialInlineLabel("Invert Height");
                ImGui::SameLine();
                if (ImGui::Checkbox("##ParallaxInvert", &materialValue.parallaxInvertHeight)) {
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("On if the map stores white as the pits rather than the peaks.\n"
                                      "If the surface looks inverted, this is the switch.");
                }
                ImGui::EndDisabled();
            }
            drawMaterialInlineLabel("Normal Strength");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##NormalMapIntensity", &materialValue.normalMapIntensity, 0.0f, 2.0f, "%.2f")) {
                materialValue.normalMapIntensity = std::clamp(materialValue.normalMapIntensity, 0.0f, 2.0f);
                changed = true;
            }
            const char* texFilterOptions[] = { "Bilinear", "Point" };
            int texFilterIndex =
                (materialValue.textureFilter == MaterialProperties::TextureFilter::Point) ? 1 : 0;
            drawMaterialInlineLabel("Filter");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##TextureFilter", &texFilterIndex, texFilterOptions, IM_ARRAYSIZE(texFilterOptions))) {
                materialValue.textureFilter =
                    (texFilterIndex == 1) ? MaterialProperties::TextureFilter::Point
                                          : MaterialProperties::TextureFilter::Bilinear;
                changed = true;
            }
            drawMaterialInlineLabel("UV Tiling");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat2("##UVTiling",
                                  &materialValue.uvTiling.x,
                                  0.01f,
                                  0.0f,
                                  0.0f,
                                  "%.2f"))
            {
                auto sanitizeUvTiling = [](float value) {
                    if (std::abs(value) >= 0.0001f) {
                        return value;
                    }
                    return (value < 0.0f) ? -0.0001f : 0.0001f;
                };
                materialValue.uvTiling.x = sanitizeUvTiling(materialValue.uvTiling.x);
                materialValue.uvTiling.y = sanitizeUvTiling(materialValue.uvTiling.y);
                changed = true;
            }
            drawMaterialInlineLabel("UV Offset");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat2("##UVOffset",
                                  &materialValue.uvOffset.x,
                                  0.01f,
                                  0.0f,
                                  0.0f,
                                  "%.2f"))
            {
                changed = true;
            }
            const MaterialShaderPreset activePreset = shaderPresetFromPaths(vertShaderPath, fragShaderPath);
            const bool usesScroll = activePreset == MaterialShaderPreset::ScrollingUV ||
                                    activePreset == MaterialShaderPreset::ProceduralClouds;
            if (usesScroll) {
                drawMaterialInlineLabel("Scroll Speed");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat("##ScrollSpeed", &materialValue.scrollSpeed, 0.01f, 0.0f, 0.0f, "%.3f")) {
                    materialValue.scrollSpeed = std::max(0.0f, materialValue.scrollSpeed);
                    changed = true;
                }
                drawMaterialInlineLabel("Scroll Direction");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat2("##ScrollDirection", &materialValue.scrollDirection.x, 0.01f, 0.0f, 0.0f, "%.2f")) {
                    changed = true;
                }
            }
        }
        if (InspectorReveal _fs = drawInspectorSubsectionFoldout(
                "Clouds", nullptr, true,
                shaderUsesCloudControls(vertShaderPath, fragShaderPath))) {
            drawMaterialInlineLabel("Cloud Color");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::ColorEdit3("##CloudColor", &materialValue.cloudColor.x)) {
                changed = true;
            }
            drawMaterialInlineLabel("Sky Color");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::ColorEdit3("##CloudSkyColor", &materialValue.cloudSkyColor.x)) {
                changed = true;
            }
            drawMaterialInlineLabel("Amount");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##CloudCoverage", &materialValue.cloudCoverage, 0.0f, 1.0f)) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Cloud coverage: 0 is clear sky, 1 is fully overcast.");
            }
            drawMaterialInlineLabel("Cloud Size");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##CloudScale", &materialValue.cloudScale, 0.05f, 0.0001f, 64.0f, "%.2f")) {
                materialValue.cloudScale = std::max(0.0001f, materialValue.cloudScale);
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How many cloud cells fit across the surface. Higher is smaller, denser puffs.");
            }
            drawMaterialInlineLabel("Softness");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##CloudSoftness", &materialValue.cloudSoftness, 0.001f, 1.0f, "%.3f")) {
                changed = true;
            }
            drawMaterialInlineLabel("Detail");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##CloudDetail", &materialValue.cloudDetail, 1, 8)) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Noise octaves. Lower is cheaper and blobbier, higher is wispier.");
            }
            drawMaterialInlineLabel("Cloud Speed");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##CloudSpeed", &materialValue.cloudSpeed, 0.005f, -4.0f, 4.0f, "%.3f")) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How fast the clouds churn in place, separate from Scroll Speed.");
            }
            drawMaterialInlineLabel("Wispiness");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##CloudWarp", &materialValue.cloudWarp, 0.0f, 2.0f, "%.2f")) {
                changed = true;
            }
            drawMaterialInlineLabel("Highlight");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##CloudHighlight", &materialValue.cloudHighlight, 1.0f, 3.0f, "%.2f")) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Extra glow in the densest cores, for a backlit look.");
            }
            drawMaterialInlineLabel("Stars");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##CloudStars", &materialValue.cloudStars, 0.0f, 1.0f, "%.2f")) {
                changed = true;
            }
            drawMaterialInlineLabel("Horizon");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##CloudHorizon", &materialValue.cloudHorizon, 0.0f, 1.0f, "%.2f")) {
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Sinks the clouds to the bottom of the surface and lights the sky where they meet.");
            }
        }
        if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Detail Maps", nullptr, true)) {
            changed |= drawMaterialTextureField("Detail Map", (std::string(idPrefix) + "Overlay").c_str(), overlayPath);
            drawMaterialInlineLabel("Detail Mix");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##DetailMix", &materialValue.textureMix, 0.0f, 1.0f)) {
                changed = true;
            }
        }
        useOverlayValue = !overlayPath.empty();

        if (spriteTarget && spriteTarget->renderType == RenderType::Sprite) {
            // Assigning through this slot also pulls in the sprite's .spritesheet clips,
            // which the plain Base Color slot above does not.
            fs::path assignedSpriteAsset;
            if (drawAssetReferenceSlot((std::string("##SpriteAsset") + idPrefix).c_str(),
                                       "Sprite Asset", albedoPath, FileCategory::Texture,
                                       "Sprite", assignedSpriteAsset)) {
                if (assignedSpriteAsset.empty()) {
                    spriteTarget->albedoTexturePath.clear();
                    spriteTarget->ui.spriteCustomFrames.clear();
                    spriteTarget->ui.spriteCustomFrameNames.clear();
                    spriteTarget->ui.spriteCustomFramesEnabled = false;
                    spriteTarget->ui.spriteSheetFrame = 0;
                    albedoPath.clear();
                    changed = true;
                } else if (assignSpriteTextureOrClips(*spriteTarget, assignedSpriteAsset)) {
                    albedoPath = spriteTarget->albedoTexturePath;
                    changed = true;
                }
            }
            ImGui::BeginDisabled(albedoPath.empty());
            if (ImGui::SmallButton((std::string("Reload Clips##") + idPrefix).c_str())) {
                if (assignSpriteTextureOrClips(*spriteTarget, fs::path(albedoPath))) {
                    albedoPath = spriteTarget->albedoTexturePath;
                    changed = true;
                }
            }
            ImGui::EndDisabled();

            if (!albedoPath.empty()) {
                ImGui::SameLine();
                if (hasSpritesheetPackage() &&
                    ImGui::SmallButton((std::string("Import Sheet##") + idPrefix).c_str())) {
                    pendingSpriteSheetPath = albedoPath;
                    std::snprintf(importSpriteSheetName, sizeof(importSpriteSheetName), "%s",
                                  spriteTarget->name.c_str());
                    importSpriteSheetTarget = isProject25DPipeline()
                        ? SpriteSheetImportTarget::Sprite25D
                        : SpriteSheetImportTarget::UIImage;
                    showImportSpriteSheetDialog = true;
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout(
                    (std::string("Sprite Sheet##") + idPrefix).c_str(), nullptr, true,
                    hasSpritesheetPackage())) {
                if (ImGui::Checkbox(Loc::Widget("INSPECTOR_ENABLE_SPRITE_SHEET", "Enable Sprite Sheet"), &spriteTarget->ui.spriteSheetEnabled)) {
                    changed = true;
                }
                ImGui::BeginDisabled(!spriteTarget->ui.spriteSheetEnabled);
                const bool usingCustomClips = spriteTarget->ui.spriteCustomFramesEnabled &&
                                              !spriteTarget->ui.spriteCustomFrames.empty();
                if (usingCustomClips) {
                    const int clipCount = static_cast<int>(spriteTarget->ui.spriteCustomFrames.size());
                    EnsureSpriteClipNames(spriteTarget->ui.spriteCustomFrameNames,
                                          spriteTarget->ui.spriteCustomFrames.size());
                    ImGui::TextDisabled("Using %d cropped sprite clips.", clipCount);
                    spriteTarget->ui.spriteSheetFrame =
                        std::clamp(spriteTarget->ui.spriteSheetFrame, 0, clipCount - 1);
                    const char* previewName =
                        spriteTarget->ui.spriteCustomFrameNames[spriteTarget->ui.spriteSheetFrame].c_str();
                    if (ImGui::BeginCombo("Clip", previewName)) {
                        for (int clipIndex = 0; clipIndex < clipCount; ++clipIndex) {
                            const bool selected = (clipIndex == spriteTarget->ui.spriteSheetFrame);
                            if (ImGui::Selectable(
                                    spriteTarget->ui.spriteCustomFrameNames[clipIndex].c_str(), selected)) {
                                spriteTarget->ui.spriteSheetFrame = clipIndex;
                                changed = true;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    int clipIndex = spriteTarget->ui.spriteSheetFrame;
                    if (ImGui::SliderInt("Clip Index", &clipIndex, 0, clipCount - 1)) {
                        spriteTarget->ui.spriteSheetFrame = std::clamp(clipIndex, 0, clipCount - 1);
                        changed = true;
                    }
                } else {
                    if (ImGui::DragInt("Columns", &spriteTarget->ui.spriteSheetColumns, 1.0f, 1, 1024)) {
                        spriteTarget->ui.spriteSheetColumns = std::max(1, spriteTarget->ui.spriteSheetColumns);
                        changed = true;
                    }
                    if (ImGui::DragInt("Rows", &spriteTarget->ui.spriteSheetRows, 1.0f, 1, 1024)) {
                        spriteTarget->ui.spriteSheetRows = std::max(1, spriteTarget->ui.spriteSheetRows);
                        changed = true;
                    }
                    const int frameCount =
                        std::max(1, spriteTarget->ui.spriteSheetColumns * spriteTarget->ui.spriteSheetRows);
                    if (ImGui::SliderInt("Frame", &spriteTarget->ui.spriteSheetFrame, 0, frameCount - 1)) {
                        spriteTarget->ui.spriteSheetFrame =
                            std::clamp(spriteTarget->ui.spriteSheetFrame, 0, frameCount - 1);
                        changed = true;
                    }
                    if (ImGui::DragFloat(Loc::Widget("INSPECTOR_FPS", "FPS"), &spriteTarget->ui.spriteSheetFps, 0.1f, 1.0f, 120.0f, "%.1f")) {
                        spriteTarget->ui.spriteSheetFps =
                            std::clamp(spriteTarget->ui.spriteSheetFps, 1.0f, 120.0f);
                        changed = true;
                    }
                    if (ImGui::Checkbox(Loc::Widget("INSPECTOR_LOOP", "Loop"), &spriteTarget->ui.spriteSheetLoop)) {
                        changed = true;
                    }
                }
                ImGui::EndDisabled();
            }
        }

        ImGui::Spacing();
        if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Preview", nullptr, true)) {
            drawMaterialPreview(
                (std::string(idPrefix) + "Preview").c_str(),
                materialValue,
                albedoPath,
                overlayPath,
                normalPath,
                useOverlayValue,
                vertShaderPath,
                fragShaderPath,
                previewScale,
                previewSlot
            );
        }

        // Reload Shader used to sit here, greyed out on every material without a custom
        // shader. It now lives in the Material header's right-click menu (and on the asset
        // panel, shown only when the material has a custom shader).

        ImGui::PopID();
        return changed;
    };

    // The asset panel edits the .mat itself, so the values are gathered back into file form to
    // push to every object using it.
    auto inspectedMaterialFileData = [&]() {
        MaterialFileData data;
        data.props = inspectedMaterial;
        data.albedo = inspectedAlbedo;
        data.overlay = inspectedOverlay;
        data.normal = inspectedNormal;
        data.metallicRoughness = inspectedMetallicRoughness;
        data.metallicMap = inspectedMetallicMap;
        data.roughnessMap = inspectedRoughnessMap;
        data.opacityMap = inspectedOpacityMap;
        data.emissive = inspectedEmissive;
        data.occlusion = inspectedOcclusion;
        data.height = inspectedHeight;
        data.useOverlay = inspectedUseOverlay;
        data.shaderPack = inspectedShaderPack;
        data.vertexShader = inspectedVertShader;
        data.fragmentShader = inspectedFragShader;
        return data;
    };
    // Material edits save themselves once the drag or text entry that made them ends, so the
    // file on disk and every object using it never disagree.
    static std::string pendingMaterialAssetSavePath;

    auto renderMaterialAssetPanel = [&](const char* headerTitle, bool allowApply) {
        if (!browserHasMaterial) return;

        ImGui::SeparatorText(headerTitle);
        if (!inspectedMaterialValid) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Failed to read material file.");
        } else {
            ImGui::TextDisabled("%s", selectedMaterialPath.filename().string().c_str());
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", selectedMaterialPath.string().c_str());
            const int materialUsers = countMaterialAssetUsers(selectedMaterialPath.string());
            if (materialUsers > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("Used by %d object%s in this scene. Edits apply to all of them.",
                                   materialUsers, materialUsers == 1 ? "" : "s");
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();

            const bool matChanged = renderMaterialEditorBody(
                "AssetMaterial",
                inspectedMaterial,
                inspectedAlbedo,
                inspectedOverlay,
                inspectedNormal,
                inspectedMetallicRoughness,
                inspectedMetallicMap,
                inspectedRoughnessMap,
                inspectedOpacityMap,
                inspectedEmissive,
                inspectedOcclusion,
                inspectedHeight,
                inspectedUseOverlay,
                inspectedShaderPack,
                inspectedVertShader,
                inspectedFragShader,
                nullptr,
                assetMaterialPreviewScale,
                1001
            );

            const std::string materialAssetPath = selectedMaterialPath.string();
            if (matChanged) {
                propagateMaterialAsset(materialAssetPath, inspectedMaterialFileData());
                pendingMaterialAssetSavePath = materialAssetPath;
            }
            if (!pendingMaterialAssetSavePath.empty() && pendingMaterialAssetSavePath == materialAssetPath &&
                !ImGui::IsAnyItemActive()) {
                if (!writeMaterialFile(inspectedMaterialFileData(),
                                       resolveMaterialAssetPath(materialAssetPath).string())) {
                    addConsoleMessage("Failed to save material: " + materialAssetPath, ConsoleMessageType::Error);
                }
                pendingMaterialAssetSavePath.clear();
            }

            if (ImGui::Button("Reload")) {
                inspectedMaterialValid = loadMaterialData(
                    selectedMaterialPath.string(),
                    inspectedMaterial,
                    inspectedAlbedo,
                    inspectedOverlay,
                    inspectedNormal,
                    inspectedUseOverlay,
                    &inspectedShaderPack,
                    &inspectedVertShader,
                    &inspectedFragShader,
                    &inspectedMetallicRoughness,
                    &inspectedMetallicMap,
                    &inspectedRoughnessMap,
                    &inspectedOpacityMap,
                    &inspectedEmissive,
                    &inspectedOcclusion,
                    &inspectedHeight
                );
                if (inspectedMaterialValid) {
                    // Picks up edits made to the file outside the editor.
                    propagateMaterialAsset(materialAssetPath, inspectedMaterialFileData());
                    pendingMaterialAssetSavePath.clear();
                }
            }
            ImGui::SetItemTooltip("Re-read the file from disk and apply it to every object using it.");
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                pendingMaterialAssetSavePath.clear();
                propagateMaterialAsset(materialAssetPath, inspectedMaterialFileData());
                if (saveMaterialData(
                        selectedMaterialPath.string(),
                        inspectedMaterial,
                        inspectedAlbedo,
                        inspectedOverlay,
                        inspectedNormal,
                        inspectedUseOverlay,
                        inspectedShaderPack,
                        inspectedVertShader,
                        inspectedFragShader,
                        &inspectedMetallicRoughness,
                        &inspectedMetallicMap,
                        &inspectedRoughnessMap,
                        &inspectedOpacityMap,
                        &inspectedEmissive,
                        &inspectedOcclusion,
                        &inspectedHeight))
                {
                    addConsoleMessage("Saved material: " + selectedMaterialPath.string(), ConsoleMessageType::Success);
                } else {
                    addConsoleMessage("Failed to save material: " + selectedMaterialPath.string(), ConsoleMessageType::Error);
                }
            }

            if (allowApply) {
                ImGui::SameLine();
                SceneObject* target = getSelectedObject();
                bool canApply = target != nullptr;
                ImGui::BeginDisabled(!canApply);
                if (ImGui::Button("Apply to Selection")) {
                    if (target) {
                        // Every map slot, not only albedo/normal: the old field-by-field copy
                        // dropped the PBR maps a material carries.
                        applyMaterialFileData(*target, inspectedMaterialFileData());
                        target->materialPath = selectedMaterialPath.string();
                        projectManager.currentProject.hasUnsavedChanges = true;
                        addConsoleMessage("Applied material to " + target->name, ConsoleMessageType::Success);
                    }
                }
                ImGui::EndDisabled();
            }
            // Only offered when there is a custom shader to reload, instead of a button that
            // is greyed out on nearly every material.
            if (!inspectedVertShader.empty() || !inspectedFragShader.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Reload Shader")) {
                    renderer.forceReloadShader(inspectedVertShader, inspectedFragShader);
                }
            }

            if (matChanged) {
                inspectedMaterialValid = true;
            }
        }
    };

    auto renderAudioAssetPanel = [&](const char* headerTitle, SceneObject* target) {
        if (!browserHasAudio) return;

        bool isPlayingPreview = audio.isPreviewing(selectedAudioPath.string());

        const ImVec4 titleColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::Spacing();
        drawAudioCenteredText(headerTitle, &titleColor);
        drawAudioCenteredText(selectedAudioPath.filename().string(), nullptr, selectedAudioPath.string().c_str());
        ImGui::Spacing();

        const float transportButton = 44.0f;
        const float transportSpacing = ImGui::GetStyle().ItemSpacing.x;
        const float transportRow = transportButton * 3.0f + transportSpacing * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()
            + std::max(0.0f, (ImGui::GetContentRegionAvail().x - transportRow) * 0.5f));

        if (drawAudioPlayerIconButton(
                "##AudioPreviewLoopButton",
                audioPreviewLoop
                    ? "Resources/Engine-Root/Audio Player/Loop Toggled On.png"
                    : "Resources/Engine-Root/Audio Player/Loop Toggled Off.png",
                "Loop",
                audioPreviewLoop ? "Disable loop" : "Enable loop",
                audioPreviewLoop,
                false,
                ImVec2(transportButton, transportButton),
                ImVec4(0.42f, 0.76f, 1.0f, 1.0f))) {
            audioPreviewLoop = !audioPreviewLoop;
            if (isPlayingPreview) {
                audio.setPreviewLoop(audioPreviewLoop);
            }
        }
        ImGui::SameLine();
        if (drawAudioPlayerIconButton(
                "##AudioPreviewPlayButton",
                isPlayingPreview
                    ? "Resources/Engine-Root/Audio Player/Play Button Toggled On.png"
                    : "Resources/Engine-Root/Audio Player/Play Button Toggled Off.png",
                "Play",
                isPlayingPreview ? "Stop preview" : "Play preview",
                isPlayingPreview,
                false,
                ImVec2(transportButton, transportButton),
                ImVec4(0.92f, 0.55f, 0.30f, 1.0f))) {
            if (isPlayingPreview) {
                stopClipPreview();
            } else {
                beginClipPreview(selectedAudioPath.string(), 1.0f, audioPreviewLoop, AudioPreviewContext::AssetBrowser);
            }
        }
        ImGui::SameLine();
        if (drawAudioPlayerIconButton(
                "##AudioPreviewAutoplayButton",
                audioPreviewAutoPlay
                    ? "Resources/Engine-Root/Audio Player/Auto Play Toggled On.png"
                    : "Resources/Engine-Root/Audio Player/Auto Play Toggled Off.png",
                "Auto",
                audioPreviewAutoPlay ? "Disable auto play" : "Enable auto play",
                audioPreviewAutoPlay,
                false,
                ImVec2(transportButton, transportButton),
                ImVec4(0.45f, 0.88f, 0.76f, 1.0f))) {
            audioPreviewAutoPlay = !audioPreviewAutoPlay;
            if (audioPreviewAutoPlay && !selectedAudioPath.empty() && !isPlayingPreview) {
                beginClipPreview(selectedAudioPath.string(), 1.0f, audioPreviewLoop, AudioPreviewContext::AssetBrowser);
            }
        }

        drawAudioPreviewVolumeControl("##AudioAssetPreviewVolume", AudioPreviewContext::AssetBrowser, 1.0f);

        if (selectedAudioPreview) {
            double cur = 0.0;
            double dur = selectedAudioPreview->durationSeconds;
            float progress = -1.0f;
            if (audio.getPreviewTime(selectedAudioPath.string(), cur, dur) && dur > 0.0001) {
                progress = static_cast<float>(cur / dur);
            }
            char formatLine[96] = {};
            std::snprintf(formatLine, sizeof(formatLine), "%u channels  |  %u Hz",
                selectedAudioPreview->channels,
                selectedAudioPreview->sampleRate);
            ImGui::Spacing();
            drawAudioCenteredText(formatLine, nullptr);
            ImVec2 waveSize(ImGui::GetContentRegionAvail().x, 96.0f);
            float seekRatio = -1.0f;
            drawWaveform("##AudioWaveAsset", selectedAudioPreview, waveSize, progress, &seekRatio);
            if (seekRatio >= 0.0f && dur > 0.0) {
                audio.seekPreview(selectedAudioPath.string(), seekRatio * dur);
            }
            ImGui::Spacing();
            drawAudioTimeReadout(cur, dur);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f), "Unable to decode audio preview.");
        }

        if (target) {
            const float buttonWidth = 146.0f;
            const float rightEdge = ImGui::GetWindowContentRegionMax().x;
            const float cursorX = ImGui::GetCursorPosX();
            if (rightEdge - buttonWidth > cursorX) {
                ImGui::SameLine(rightEdge - buttonWidth);
            } else {
                ImGui::SameLine();
            }
            if (ImGui::Button("Assign to Selection", ImVec2(buttonWidth, 0.0f))) {
                if (!target->hasAudioSource) {
                    target->hasAudioSource = true;
                    target->audioSource = AudioSourceComponent{};
                }
                target->audioSource.clipPath = selectedAudioPath.string();
                projectManager.currentProject.hasUnsavedChanges = true;
            }
        }
    };

    auto renderTextureAssetPanel = [&](const char* headerTitle) {
        if (!browserHasTexture) return;

        ImGui::SeparatorText(headerTitle);

        ImGui::TextDisabled("%s", selectedTexturePath.filename().string().c_str());
        ImGui::TextColored(ImVec4(0.8f, 0.65f, 0.95f, 1.0f), "%s", selectedTexturePath.string().c_str());

        static float textureAssetPreviewZoom = 1.0f;
        Texture* previewTex = renderer.getTexture(selectedTexturePath.string(), MaterialProperties::TextureFilter::Point);

        ImGui::Spacing();
        if (previewTex && previewTex->GetID()) {
            ImGui::SliderFloat(Loc::Widget("INSPECTOR_PREVIEW_ZOOM", "Preview Zoom"), &textureAssetPreviewZoom, 0.25f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
            float maxWidth = ImGui::GetContentRegionAvail().x;
            float size = std::min(maxWidth, 160.0f * textureAssetPreviewZoom);
            float aspect = previewTex->GetHeight() > 0 ? (previewTex->GetWidth() / static_cast<float>(previewTex->GetHeight())) : 1.0f;
            ImVec2 imageSize(size, size);
            if (aspect > 1.0f) {
                imageSize.y = size / aspect;
            } else if (aspect > 0.0f) {
                imageSize.x = size * aspect;
            }
            ImGui::Image((ImTextureID)(intptr_t)previewTex->GetID(), imageSize, ImVec2(0, 1), ImVec2(1, 0));
            ImGui::Text("Size: %d x %d", previewTex->GetWidth(), previewTex->GetHeight());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "Unable to load texture preview.");
        }
    };

    auto renderVideoAssetPanel = [&](const char* headerTitle) {
        if (!browserHasVideo) return;

        ImGui::SeparatorText(headerTitle);
        ImGui::TextDisabled("%s", selectedVideoPath.filename().string().c_str());
        drawTrimmedPathText(selectedVideoPath.string(), ImVec4(0.92f, 0.72f, 0.78f, 1.0f));
        ImGui::Spacing();

        if (!videoAssetPreviewPlayer) {
            ImGui::TextDisabled("Preview is loaded on demand.");
        }

        if (ImGui::Button("Play", ImVec2(72.0f, 0.0f))) {
            if (!videoAssetPreviewPlayer && !videoAssetPreviewPath.empty()) {
                videoAssetPreviewPlayer = std::make_unique<VideoPlayer>();
                videoAssetPreviewPlayer->SetLoop(videoAssetPreviewLoop);
                if (!videoAssetPreviewPlayer->LoadVideo(videoAssetPreviewPath)) {
                    videoAssetPreviewPlayer.reset();
                }
            }
            if (videoAssetPreviewPlayer) {
                videoAssetPreviewPlayer->SetLoop(videoAssetPreviewLoop);
                videoAssetPreviewPlayer->Play();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(72.0f, 0.0f))) {
            if (videoAssetPreviewPlayer) {
                videoAssetPreviewPlayer->Stop();
                videoAssetPreviewPlayer.reset();
            }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(Loc::Widget("INSPECTOR_LOOP_2", "Loop"), &videoAssetPreviewLoop) && videoAssetPreviewPlayer) {
            videoAssetPreviewPlayer->SetLoop(videoAssetPreviewLoop);
        }

        if (videoAssetPreviewPlayer && !videoAssetPreviewPlayer->GetLastError().empty() && !videoAssetPreviewPlayer->IsLoaded()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "%s", videoAssetPreviewPlayer->GetLastError().c_str());
            return;
        }

        if (videoAssetPreviewPlayer && videoAssetPreviewPlayer->IsLoaded()) {
            ImGui::TextDisabled("Duration: %.2fs", videoAssetPreviewPlayer->GetDurationSeconds());
            ImGui::TextDisabled("Resolution: %d x %d",
                                videoAssetPreviewPlayer->GetWidth(),
                                videoAssetPreviewPlayer->GetHeight());
            ImGui::TextDisabled("Preview audio is not implemented.");
        }

        ImGui::Spacing();
        if (videoAssetPreviewPlayer &&
            videoAssetPreviewPlayer->HasTextureOverride() &&
            videoAssetPreviewPlayer->GetTextureId() != 0 &&
            videoAssetPreviewPlayer->GetWidth() > 0 &&
            videoAssetPreviewPlayer->GetHeight() > 0) {
            static float videoAssetPreviewZoom = 1.0f;
            ImGui::SliderFloat(Loc::Widget("INSPECTOR_PREVIEW_ZOOM_2", "Preview Zoom"), &videoAssetPreviewZoom, 0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);

            const float maxWidth = ImGui::GetContentRegionAvail().x;
            const float baseWidth = 240.0f * videoAssetPreviewZoom;
            const float width = std::min(maxWidth, baseWidth);
            const float aspect = static_cast<float>(videoAssetPreviewPlayer->GetWidth()) /
                                 static_cast<float>(std::max(1, videoAssetPreviewPlayer->GetHeight()));
            ImVec2 imageSize(width, width / std::max(0.0001f, aspect));
            ImGui::Image((ImTextureID)(intptr_t)videoAssetPreviewPlayer->GetTextureId(),
                         imageSize,
                         ImVec2(0, 0),
                         ImVec2(1, 1));
        } else if (videoAssetPreviewPlayer && videoAssetPreviewPlayer->IsLoaded()) {
            ImGui::TextDisabled("Press Play to preview the video.");
        }
    };

    if (selectedObjectIds.empty()) {
        if (assetPreviewSuppressed && (browserHasMaterial || browserHasAudio || browserHasTexture || browserHasVideo)) {
            ImGui::TextDisabled("Asset previews are disabled while the scene is running.");
            ImGui::Spacing();
            ImGui::TextDisabled("Select an object to inspect components, or stop playback to edit assets.");
        } else if (browserHasMaterial) {
            renderMaterialAssetPanel("Material Asset", true);
        } else if (browserHasAudio) {
            renderAudioAssetPanel("Audio Clip", nullptr);
        } else if (browserHasVideo) {
            renderVideoAssetPanel("Video Asset");
        } else if (browserHasTexture) {
            renderTextureAssetPanel("Texture");
        } else {
            ImGui::TextDisabled("No object selected");
        }
        if (inspectorWrapText) ImGui::PopTextWrapPos();
        ImGui::PopStyleVar(3);
        ImGui::End();
        return;
    }

    int primaryId = selectedObjectId;
    auto it = std::find_if(sceneObjects.begin(), sceneObjects.end(),
        [primaryId](const SceneObject& obj) { return obj.id == primaryId; });

    if (it == sceneObjects.end()) {
        ImGui::TextDisabled("Object not found");
        if (inspectorWrapText) ImGui::PopTextWrapPos();
        ImGui::PopStyleVar(3);
        ImGui::End();
        return;
    }

    SceneObject& obj = *it;
    ImGui::PushID(obj.id); // Scope per-object widgets to avoid ID collisions
    auto isUIObject = [](const SceneObject& target) {
        return target.hasUI && target.ui.type != UIElementType::None;
    };

    std::vector<SceneObject*> selectedObjects;
    selectedObjects.reserve(selectedObjectIds.size() + 1);
    std::unordered_set<int> seenSelectedIds;
    for (int id : selectedObjectIds) {
        if (id < 0 || seenSelectedIds.count(id) > 0) continue;
        if (SceneObject* selectedObj = findObjectById(id)) {
            selectedObjects.push_back(selectedObj);
            seenSelectedIds.insert(id);
        }
    }
    if (seenSelectedIds.count(obj.id) == 0) {
        selectedObjects.push_back(&obj);
    }

    const bool multiSelection = selectedObjects.size() > 1;

    auto allSelected = [&](auto&& predicate) {
        if (selectedObjects.empty()) return false;
        for (const SceneObject* selectedObj : selectedObjects) {
            if (!selectedObj || !predicate(*selectedObj)) return false;
        }
        return true;
    };
    auto hasMixedSelected = [&](auto&& predicate) {
        bool any = false;
        bool all = true;
        for (const SceneObject* selectedObj : selectedObjects) {
            if (!selectedObj) continue;
            const bool value = predicate(*selectedObj);
            any = any || value;
            all = all && value;
        }
        return any && !all;
    };

    const bool sharedUIObject = allSelected([&](const SceneObject& candidate) { return isUIObject(candidate); });
    const bool sharedCollider = allSelected([](const SceneObject& candidate) { return candidate.hasCollider; });
    const bool sharedPlayerController = allSelected([](const SceneObject& candidate) { return candidate.hasPlayerController; });
    const bool sharedRigidbody = allSelected([](const SceneObject& candidate) { return candidate.hasRigidbody; });
    const bool sharedRigidbody2D = allSelected([](const SceneObject& candidate) { return candidate.hasRigidbody2D; });
    const bool sharedCollider2D = allSelected([](const SceneObject& candidate) { return candidate.hasCollider2D; });
    const bool sharedParallax2D = allSelected([](const SceneObject& candidate) { return candidate.hasParallaxLayer2D; });
    const bool sharedAssemblage = allSelected([](const SceneObject& candidate) { return candidate.hasAssemblage; });
    const bool sharedAssemblageLayer = allSelected([](const SceneObject& candidate) { return candidate.hasAssemblageLayer; });
    const bool sharedAudioSource = allSelected([](const SceneObject& candidate) { return candidate.hasAudioSource; });
    const bool sharedAudioFX = allSelected([](const SceneObject& candidate) { return candidate.hasAudioFX; });
    const bool sharedVideoPlayer = allSelected([](const SceneObject& candidate) { return candidate.hasVideoPlayer; });
    const bool sharedParticleSystem2D = allSelected([](const SceneObject& candidate) { return candidate.hasParticleSystem2D; });
    const bool sharedGroundBaked = allSelected([](const SceneObject& candidate) { return candidate.hasGroundBakedType; });
    const bool sharedObstacle = allSelected([](const SceneObject& candidate) { return candidate.hasObsticleObject; });
    const bool sharedAgent = allSelected([](const SceneObject& candidate) { return candidate.hasAIAgent; });
    const bool sharedOffMeshLink = allSelected([](const SceneObject& candidate) { return candidate.hasOffMeshLink; });
    const bool sharedMapRoot = allSelected([](const SceneObject& candidate) { return candidate.hasMapRoot; });
    const bool sharedMapSector = allSelected([](const SceneObject& candidate) { return candidate.hasMapSector; });
    const bool sharedMapTransition = allSelected([](const SceneObject& candidate) { return candidate.hasMapTransition; });
    const bool sharedMapPortal = allSelected([](const SceneObject& candidate) { return candidate.hasMapPortal; });
    const bool sharedMapMesh = allSelected([](const SceneObject& candidate) { return candidate.hasMapMesh; });
    const bool sharedAnimation = allSelected([](const SceneObject& candidate) { return candidate.hasAnimation; });
    const bool sharedSkeletal = allSelected([](const SceneObject& candidate) { return candidate.hasSkeletalAnimation; });
    const bool sharedReverb = allSelected([](const SceneObject& candidate) { return candidate.hasReverbZone; });
    const bool sharedXROrigin = allSelected([](const SceneObject& candidate) { return candidate.hasXROrigin; });
    const bool sharedXRCamera = allSelected([](const SceneObject& candidate) { return candidate.hasXRCamera; });
    const bool sharedXRController = allSelected([](const SceneObject& candidate) { return candidate.hasXRController; });
    const bool sharedXRActionController = allSelected([](const SceneObject& candidate) { return candidate.hasXRActionBasedController; });
    const bool sharedXRRayInteractor = allSelected([](const SceneObject& candidate) { return candidate.hasXRRayInteractor; });
    const bool sharedXRDirectInteractor = allSelected([](const SceneObject& candidate) { return candidate.hasXRDirectInteractor; });
    const bool sharedXRGrabInteractable = allSelected([](const SceneObject& candidate) { return candidate.hasXRGrabInteractable; });
    const bool sharedCamera = allSelected([](const SceneObject& candidate) { return candidate.hasCamera; });
    const bool sharedCameraFollow2D = allSelected([](const SceneObject& candidate) { return candidate.hasCameraFollow2D; });
    const bool sharedPostFX = allSelected([](const SceneObject& candidate) { return candidate.hasPostFX; });
    const bool sharedRenderer = allSelected([](const SceneObject& candidate) { return candidate.hasRenderer; });
    const bool sharedLight = allSelected([](const SceneObject& candidate) { return candidate.hasLight; });
    const bool sharedLight2D = allSelected([](const SceneObject& candidate) { return candidate.hasLight2D; });
    const bool sharedReflectionCast = allSelected([](const SceneObject& candidate) { return candidate.hasReflectionCast; });
    const bool sharedShadowCaster2D = allSelected([](const SceneObject& candidate) { return candidate.hasShadowCaster2D; });

    auto scriptSignature = [](const ScriptComponent& script) {
        return std::to_string(static_cast<int>(script.language)) + "|" + script.path + "|" + script.managedType;
    };
    auto hasScriptSignature = [&](const SceneObject& candidate, const std::string& signature) {
        for (const ScriptComponent& script : candidate.scripts) {
            if (scriptSignature(script) == signature) return true;
        }
        return false;
    };
    const bool sharedScriptsLayout = allSelected([&](const SceneObject& candidate) {
        if (candidate.scripts.size() != obj.scripts.size()) return false;
        for (size_t i = 0; i < obj.scripts.size(); ++i) {
            if (scriptSignature(candidate.scripts[i]) != scriptSignature(obj.scripts[i])) {
                return false;
            }
        }
        return true;
    });
    std::vector<bool> sharedScriptByIndex(obj.scripts.size(), !multiSelection);
    std::vector<std::string> sharedScriptSignatures(obj.scripts.size());
    if (multiSelection) {
        for (size_t i = 0; i < obj.scripts.size(); ++i) {
            const std::string signature = scriptSignature(obj.scripts[i]);
            sharedScriptSignatures[i] = signature;
            bool shared = true;
            for (const SceneObject* selectedObj : selectedObjects) {
                if (!selectedObj || selectedObj->id == obj.id) continue;
                if (!hasScriptSignature(*selectedObj, signature)) {
                    shared = false;
                    break;
                }
            }
            sharedScriptByIndex[i] = shared;
        }
    } else {
        for (size_t i = 0; i < obj.scripts.size(); ++i) {
            sharedScriptSignatures[i] = scriptSignature(obj.scripts[i]);
        }
    }

    const bool hasMixedComponents = multiSelection && (
        hasMixedSelected([&](const SceneObject& candidate) { return isUIObject(candidate); }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasCollider; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasPlayerController; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasRigidbody; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasRigidbody2D; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasCollider2D; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasParallaxLayer2D; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasAudioSource; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasAudioFX; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasVideoPlayer; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasGroundBakedType; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasObsticleObject; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasAIAgent; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasOffMeshLink; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasAnimation; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasSkeletalAnimation; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasReverbZone; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasCamera; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasCameraFollow2D; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasPostFX; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasRenderer; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasLight; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasLight2D; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasReflectionCast; }) ||
        hasMixedSelected([](const SceneObject& candidate) { return candidate.hasShadowCaster2D; }) ||
        !sharedScriptsLayout
    );

    if (multiSelection) {
        ImGui::Text("Multiple objects selected: %zu", selectedObjects.size());
        ImGui::Separator();
    }

    auto formatInspectorMixedValue = [&](const auto& value, const char* floatFormat = "%.3f") -> std::string {
        using ValueT = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<ValueT, std::string>) {
            return value.empty() ? "<empty>" : value;
        } else if constexpr (std::is_same_v<ValueT, const char*>) {
            return (value && value[0] != '\0') ? std::string(value) : std::string("<empty>");
        } else if constexpr (std::is_same_v<ValueT, bool>) {
            return value ? "true" : "false";
        } else if constexpr (std::is_floating_point_v<ValueT>) {
            char buf[64] = {};
            std::snprintf(buf, sizeof(buf), floatFormat ? floatFormat : "%.3f", static_cast<double>(value));
            return buf;
        } else if constexpr (std::is_integral_v<ValueT>) {
            return std::to_string(value);
        } else if constexpr (std::is_enum_v<ValueT>) {
            return std::to_string(static_cast<int>(value));
        } else {
            return "<value>";
        }
    };

    auto drawMixedValuePopup = [&](const char* id,
                                   const auto& currentValue,
                                   auto&& getter,
                                   auto&& assignValue,
                                   auto&& clearValue,
                                   const char* floatFormat = "%.3f") -> bool {
        if (!multiSelection) {
            return false;
        }

        std::string buttonId = std::string("--##MixedValue_") + id;
        bool changed = false;
        if (ImGui::Button(buttonId.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
            ImGui::OpenPopup((std::string("##MixedValuePopup_") + id).c_str());
        }
        if (ImGui::BeginPopup((std::string("##MixedValuePopup_") + id).c_str())) {
            ImGui::TextUnformatted("Which value would you like to change it to?");
            ImGui::Separator();
            for (SceneObject* selectedObj : selectedObjects) {
                if (!selectedObj) continue;
                const auto value = getter(*selectedObj);
                std::string label = selectedObj->name + " (" + formatInspectorMixedValue(value, floatFormat) + ")##" +
                    std::to_string(selectedObj->id);
                if (ImGui::Selectable(label.c_str())) {
                    assignValue(value);
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Separator();
            if (ImGui::Selectable("Clear Value")) {
                clearValue();
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Mixed values");
        }
        (void)currentValue;
        return changed;
    };

    auto selectedValuesMixed = [&](auto&& getter) {
        if (!multiSelection || selectedObjects.empty()) return false;
        bool haveFirst = false;
        using ValueT = std::decay_t<decltype(getter(*selectedObjects.front()))>;
        ValueT first{};
        for (SceneObject* selectedObj : selectedObjects) {
            if (!selectedObj) continue;
            const ValueT value = getter(*selectedObj);
            if (!haveFirst) {
                first = value;
                haveFirst = true;
                continue;
            }
            if (value != first) {
                return true;
            }
        }
        return false;
    };

    auto mixedInputText = [&](const char* id,
                              char* buffer,
                              size_t bufferSize,
                              auto&& getter,
                              auto&& setter,
                              const char* clearValue = "") -> bool {
        if (selectedValuesMixed(getter)) {
            return drawMixedValuePopup(id,
                std::string(buffer),
                getter,
                [&](const std::string& value) {
                    std::snprintf(buffer, bufferSize, "%s", value.c_str());
                    setter(value);
                },
                [&]() {
                    std::snprintf(buffer, bufferSize, "%s", clearValue);
                    setter(std::string(clearValue));
                });
        }
        if (ImGui::InputText(id, buffer, bufferSize)) {
            setter(std::string(buffer));
            return true;
        }
        return false;
    };

    auto mixedCheckbox = [&](const char* id,
                             bool* value,
                             auto&& getter,
                             auto&& setter) -> bool {
        if (selectedValuesMixed(getter)) {
            return drawMixedValuePopup(id,
                *value,
                getter,
                [&](bool selectedValue) {
                    *value = selectedValue;
                    setter(selectedValue);
                },
                [&]() {
                    *value = false;
                    setter(false);
                });
        }
        if (ImGui::Checkbox(id, value)) {
            setter(*value);
            return true;
        }
        return false;
    };

    auto mixedSliderInt = [&](const char* id,
                              int* value,
                              int minValue,
                              int maxValue,
                              const char* format,
                              auto&& getter,
                              auto&& setter) -> bool {
        if (selectedValuesMixed(getter)) {
            return drawMixedValuePopup(id,
                *value,
                getter,
                [&](int selectedValue) {
                    *value = std::clamp(selectedValue, minValue, maxValue);
                    setter(*value);
                },
                [&]() {
                    *value = minValue;
                    setter(*value);
                });
        }
        if (ImGui::SliderInt(id, value, minValue, maxValue, format)) {
            setter(*value);
            return true;
        }
        return false;
    };

    auto mixedCombo = [&](const char* id,
                          int* value,
                          const char* const labels[],
                          int labelCount,
                          auto&& getter,
                          auto&& setter) -> bool {
        if (selectedValuesMixed(getter)) {
            auto labelGetter = [&](const SceneObject& selectedObj) -> std::string {
                const int index = static_cast<int>(getter(selectedObj));
                if (index >= 0 && index < labelCount) return labels[index];
                return std::to_string(index);
            };
            return drawMixedValuePopup(id,
                std::string("--"),
                labelGetter,
                [&](const std::string& selectedLabel) {
                    for (int i = 0; i < labelCount; ++i) {
                        if (selectedLabel == labels[i]) {
                            *value = i;
                            setter(i);
                            break;
                        }
                    }
                },
                [&]() {
                    *value = 0;
                    setter(0);
                });
        }
        if (ImGui::Combo(id, value, labels, labelCount)) {
            setter(*value);
            return true;
        }
        return false;
    };

    auto mixedDragFloat = [&](const char* id,
                              float* value,
                              float speed,
                              float minValue,
                              float maxValue,
                              const char* format,
                              auto&& getter,
                              auto&& setter,
                              float clearValue = 0.0f) -> bool {
        if (selectedValuesMixed(getter)) {
            return drawMixedValuePopup(id,
                *value,
                getter,
                [&](float selectedValue) {
                    *value = selectedValue;
                    setter(selectedValue);
                },
                [&]() {
                    *value = clearValue;
                    setter(clearValue);
                },
                format);
        }
        if (ImGui::DragFloat(id, value, speed, minValue, maxValue, format)) {
            setter(*value);
            return true;
        }
        return false;
    };

    auto mixedDragFloatN = [&](const char* id,
                               float* values,
                               int count,
                               float speed,
                               float minValue,
                               float maxValue,
                               const char* format,
                               auto&& getter,
                               auto&& setter,
                               float clearValue = 0.0f) -> bool {
        bool changed = false;
        ImGui::PushID(id);
        ImGui::PushMultiItemsWidths(count, ImGui::CalcItemWidth());
        for (int component = 0; component < count; ++component) {
            if (component > 0) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::PushID(component);
            auto componentGetter = [&](const SceneObject& selectedObj) {
                return getter(selectedObj, component);
            };
            auto componentSetter = [&](float selectedValue) {
                values[component] = selectedValue;
                setter(component, selectedValue);
            };
            changed |= mixedDragFloat("##v", &values[component], speed, minValue, maxValue, format,
                                      componentGetter, componentSetter, clearValue);
            ImGui::PopID();
            ImGui::PopItemWidth();
        }
        // The per-component widgets are all "##v", so the visible label has to be drawn here,
        // the way DragFloatN does; without it "Position (px)" and "Size (px)" had no label column.
        const char* labelEnd = ImGui::FindRenderedTextEnd(id);
        if (labelEnd != id) {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextUnformatted(id, labelEnd);
        }
        ImGui::PopID();
        return changed;
    };

    bool objectNameChanged = false;
    bool objectEnabledChanged = false;
    bool objectInvariableChanged = false;
    bool objectIconChanged = false;
    bool objectLayerChanged = false;
    bool objectTagChanged = false;
    bool objectTransformChanged = false;
    bool uiSectionChanged = false;
    bool colliderSectionChanged = false;
    bool playerControllerSectionChanged = false;
    bool rigidbodySectionChanged = false;
    bool rigidbody2DSectionChanged = false;
    bool collider2DSectionChanged = false;
    bool parallax2DSectionChanged = false;
    bool audioSourceSectionChanged = false;
    bool audioFXSectionChanged = false;
    bool videoPlayerSectionChanged = false;
    bool particleSystem2DSectionChanged = false;
    bool groundBakedSectionChanged = false;
    bool obstacleSectionChanged = false;
    bool agentSectionChanged = false;
    bool animationSectionChanged = false;
    bool skeletalSectionChanged = false;
    bool reverbSectionChanged = false;
    bool cameraSectionChanged = false;
    bool cameraFollowSectionChanged = false;
    bool postFxSectionChanged = false;
    bool rendererSectionChanged = false;
    bool lightSectionChanged = false;
    bool light2DSectionChanged = false;
    bool shadowCaster2DSectionChanged = false;
    bool scriptsChanged = false;
    int scriptToRemove = -1;
    int scriptPasteInsertAfter = -1;
    ScriptComponent scriptPasteInsertValue{};
    std::string scriptPasteInsertAnchorKey;
    bool inspectorOrderChanged = false;
    EnsureInspectorComponentMetadata(obj);
    static bool inspectorHistoryPending = false;
    static SceneSnapshot inspectorHistoryBefore;

    // Inspector undo needs the scene as it was *before* an edit, but a SceneSnapshot deep-copies
    // every SceneObject (5.8 KB each, plus every non-empty string/keyframe/bone-matrix buffer) and
    // the mesh-edit asset. Capturing one unconditionally meant paying that on every frame an object
    // was selected and throwing it away on the frames nothing was edited, which is nearly all of
    // them. An inspector widget can only report a change while the user is driving this panel, and
    // the snapshot is only consumed on the first changed frame (see inspectorHistoryPending below),
    // so capture it only when both of those hold.
    const ImGuiIO& inspectorIo = ImGui::GetIO();
    bool inspectorInputActivity =
        ImGui::IsAnyItemActive() ||                   // drag/slider/input already being manipulated
        ImGui::GetDragDropPayload() != nullptr ||     // asset drop in flight
        inspectorIo.InputQueueCharacters.Size > 0 ||  // typing into a field
        inspectorIo.KeyCtrl;                          // ctrl+arrow component reordering
    for (int button = 0; button < ImGuiMouseButton_COUNT && !inspectorInputActivity; ++button) {
        // Edges only: a held button belongs to whatever took it (usually a viewport camera drag),
        // and a widget that reports a change on a held button is already active above.
        inspectorInputActivity = ImGui::IsMouseClicked(button) || ImGui::IsMouseReleased(button);
    }
    const bool inspectorTargeted =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                               ImGuiHoveredFlags_AllowWhenBlockedByPopup) ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) ||
        ImGui::GetDragDropPayload() != nullptr;
    const bool inspectorFrameBeforeValid =
        !inspectorHistoryPending && inspectorInputActivity && inspectorTargeted;
    SceneSnapshot inspectorFrameBefore;
    if (inspectorFrameBeforeValid) {
        inspectorFrameBefore = captureSceneSnapshot();
    }

    // Multi-select propagation diffs the primary object against its own pre-frame state. That needs
    // one object rather than the whole scene, and it has to stay available on the frames the undo
    // snapshot above is skipped.
    std::optional<SceneObject> inspectorPrimaryBeforeStorage;
    const SceneObject* inspectorPrimaryBefore = nullptr;
    if (multiSelection) {
        inspectorPrimaryBeforeStorage = obj;
        inspectorPrimaryBefore = &*inspectorPrimaryBeforeStorage;
    }

    const bool inspectorKeysActive =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput;
    const bool componentContextMenuKey = inspectorKeysActive &&
        (ImGui::IsKeyPressed(ImGuiKey_Menu, false) ||
         (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false)));

    auto componentIconForId = [&](const std::string& key) -> InspectorUiIcon {
        if (key == "Camera" || key == "CameraFollow2D") return iconCompCamera;
        if (key == "Light" || key == "Light2D" || key == "ShadowCaster2D") return iconCompLight;
        if (key == "PostFX") return iconCompVolume;
        if (key == "AudioSource" || key == "ReverbZone" || key == "AudioFX") return iconCompAudio;
        if (key == "Renderer") return iconCompMesh;
        if (key == "Collider" || key == "Collider2D") return iconCompCollider;
        if (key == "Rigidbody3D" || key == "Rigidbody2D") return iconCompRigidbody;
        if (key == "ParticleSystem2D") return iconCompParticle;
        if (key == "VideoPlayer") return iconCompVideo;
        if (key == "NetworkManager" || key == "NetworkIdentity") return iconCompNetwork;
        if (key == "UI") {
            switch (obj.ui.type) {
                case UIElementType::Text: return iconCompText;
                case UIElementType::Image:
                case UIElementType::Sprite2D: return iconCompSprite;
                default: return iconCompCanvas;
            }
        }
        return iconUnknownComponent;
    };

    auto drawComponentHeader = [&](const char* label,
                                   const char* id,
                                   const std::string& reorderKey,
                                   bool* enabled,
                                   bool defaultOpen,
                                   const std::function<void()>& menuFn,
                                   const InspectorUiIcon& icon = {}) -> ComponentHeaderState {
        ComponentHeaderState state;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (defaultOpen) {
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        InspectorUiIcon headerIcon = icon;
        if (headerIcon.id == static_cast<ImTextureID>(0)) {
            headerIcon = componentIconForId(id ? id : "");
        }
        std::string headerId;
        if (headerIcon.id != static_cast<ImTextureID>(0)) {
            const float spaceW = std::max(1.0f, ImGui::CalcTextSize(" ").x);
            const float maxIconSpan = std::max(8.0f, ImGui::GetFrameHeight() - 9.0f);
            const float drawnIconSize = (headerIcon.nativeW > 0.0f)
                                            ? std::min(headerIcon.nativeW, maxIconSpan)
                                            : maxIconSpan;
            const float iconSpan = drawnIconSize + 6.0f;
            headerId.assign(static_cast<size_t>(std::ceil(iconSpan / spaceW)), ' ');
        }
        headerId += label;
        headerId += "##";
        headerId += id;
        ImGui::SetNextItemAllowOverlap();
        ImGuiStyle& style = ImGui::GetStyle();

        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImDrawList* plateDl = ImGui::GetWindowDrawList();
        plateDl->ChannelsSplit(2);
        plateDl->ChannelsSetCurrent(1);

        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(style.FramePadding.x, style.FramePadding.y + 2.0f));
        const bool headerActuallyOpen = ImGui::CollapsingHeader(headerId.c_str(), flags);
        state.open = headerActuallyOpen;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        const bool headerFocused = ImGui::IsItemFocused();
        const ImGuiID revealKey = ImGui::GetID((headerId + "##body").c_str());
        inspectorPendingRevealId = revealKey;
        inspectorPendingRevealOpen = headerActuallyOpen;
        state.open = headerActuallyOpen ||
                     editorUiAnimationStates[revealKey].foldOpen > 0.002f;

        {
            const bool headerHovered = ImGui::IsItemHovered();
            const bool headerHeld = ImGui::IsItemActive();
            const ImGuiShadeState shadeState = headerHeld    ? ImGuiShadeState_Active
                                               : headerHovered ? ImGuiShadeState_Hovered
                                                               : ImGuiShadeState_Normal;
            const ImGuiCol fillCol = headerHeld    ? ImGuiCol_ButtonActive
                                     : headerHovered ? ImGuiCol_ButtonHovered
                                                     : ImGuiCol_Button;
            const ImVec2 plateMin = ImGui::GetItemRectMin();
            const ImVec2 plateMax = ImGui::GetItemRectMax();
            plateDl->ChannelsSetCurrent(0);
            ImGui::ShadeRect(plateDl, plateMin, plateMax,
                             ImGui::GetColorU32(fillCol), ImGuiShadeClass_Button,
                             shadeState, style.FrameRounding);
            plateDl->ChannelsMerge();

            if (iconCollapseArrow.id != static_cast<ImTextureID>(0)) {
                const float fontSize = ImGui::GetFontSize();
                const float slotX = plateMin.x + style.FramePadding.x;
                const float slotRight = slotX + fontSize;
                plateDl->PushClipRect(ImVec2(plateMin.x, plateMin.y),
                                      ImVec2(slotRight + 1.0f, plateMax.y), true);
                ImGui::ShadeRect(plateDl, plateMin, plateMax,
                                 ImGui::GetColorU32(fillCol), ImGuiShadeClass_Button,
                                 shadeState, style.FrameRounding);
                plateDl->PopClipRect();

                float animSpeed = 0.0f;
                if (uiAnimationMode == UIAnimationMode::Fluid) animSpeed = 12.0f;
                else if (uiAnimationMode == UIAnimationMode::Snappy) animSpeed = 22.0f;
                const float animStep =
                    (uiAnimationMode == UIAnimationMode::Off)
                        ? 1.0f
                        : (1.0f - std::exp(-animSpeed * ImGui::GetIO().DeltaTime));
                UIAnimationState& foldState = editorUiAnimationStates[ImGui::GetID(headerId.c_str())];
                const float target = state.open ? 1.0f : 0.0f;
                if (!foldState.initialized) {
                    foldState.foldOpen = target;
                    foldState.initialized = true;
                } else {
                    foldState.foldOpen += (target - foldState.foldOpen) * animStep;
                }
                if (std::abs(foldState.foldOpen - target) < 0.001f) {
                    foldState.foldOpen = target;
                }

                const float side = (iconCollapseArrow.nativeW > 0.0f)
                                       ? iconCollapseArrow.nativeW
                                       : std::floor(fontSize);
                const bool atRest = (foldState.foldOpen <= 0.0f || foldState.foldOpen >= 1.0f);
                ImVec2 center((slotX + slotRight) * 0.5f,
                              (plateMin.y + plateMax.y) * 0.5f);
                if (atRest) {
                    center.x = std::floor(center.x - side * 0.5f) + side * 0.5f;
                    center.y = std::floor(center.y - side * 0.5f) + side * 0.5f;
                }
                const float angle = foldState.foldOpen * 1.57079633f;
                const float halfSide = side * 0.5f;
                const float cosA = std::cos(angle);
                const float sinA = std::sin(angle);
                auto corner = [&](float x, float y) {
                    return ImVec2(center.x + x * cosA - y * sinA,
                                  center.y + x * sinA + y * cosA);
                };
                ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 0.0f), uv2(1.0f, 1.0f), uv3(0.0f, 1.0f);
                if (iconCollapseArrow.flipY) {
                    uv0 = ImVec2(0.0f, 1.0f); uv1 = ImVec2(1.0f, 1.0f);
                    uv2 = ImVec2(1.0f, 0.0f); uv3 = ImVec2(0.0f, 0.0f);
                }
                plateDl->AddImageQuad(iconCollapseArrow.id,
                                      corner(-halfSide, -halfSide),
                                      corner(halfSide, -halfSide),
                                      corner(halfSide, halfSide),
                                      corner(-halfSide, halfSide),
                                      uv0, uv1, uv2, uv3,
                                      IM_COL32(255, 255, 255, 235));
            }
        }

        if (headerFocused && !reorderKey.empty() && ImGui::GetIO().KeyCtrl) {
            const bool toEdge = ImGui::GetIO().KeyShift;
            bool moved = false;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
                moved = toEdge ? MoveInspectorComponentToEdge(obj, reorderKey, true)
                               : MoveInspectorComponentByOffset(obj, reorderKey, -1);
            } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
                moved = toEdge ? MoveInspectorComponentToEdge(obj, reorderKey, false)
                               : MoveInspectorComponentByOffset(obj, reorderKey, 1);
            }
            if (moved) {
                inspectorOrderChanged = true;
                projectManager.currentProject.hasUnsavedChanges = true;
            }
        }

        const ImVec2 headerMin = ImGui::GetItemRectMin();
        const ImVec2 headerMax = ImGui::GetItemRectMax();
        const ImVec2 cursorAfter = ImGui::GetCursorScreenPos();
        const float headerHeight = headerMax.y - headerMin.y;
        const float controlSize = ImGui::GetFrameHeight();
        float right = headerMax.x - style.FramePadding.x;

        if (!reorderKey.empty()) {
            if (DragPreview::BeginSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
                ImGui::SetDragDropPayload("INSPECTOR_COMPONENT", reorderKey.c_str(), reorderKey.size() + 1);
                DragPreview::SubmitMeta(label, (ImTextureID)0, "INSPECTOR_COMPONENT");
                DragPreview::EndSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INSPECTOR_COMPONENT")) {
                    const char* payloadKey = static_cast<const char*>(payload->Data);
                    if (payloadKey && MoveInspectorComponentBefore(obj, payloadKey, reorderKey)) {
                        inspectorOrderChanged = true;
                        projectManager.currentProject.hasUnsavedChanges = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        ImGui::PushID(id);
        if (menuFn) {
            const ImVec2 menuPos(right - controlSize, headerMin.y + (headerHeight - controlSize) * 0.5f);
            ImGui::SetCursorScreenPos(menuPos);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.25f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            bool menuClicked = false;
            if (iconActionsMenu.id != static_cast<ImTextureID>(0)) {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
                const ImVec2 uvMin = iconActionsMenu.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
                const ImVec2 uvMax = iconActionsMenu.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
                menuClicked = ImGui::ImageButton("##menu", iconActionsMenu.id, ImVec2(controlSize - 2.0f, controlSize - 2.0f), uvMin, uvMax);
                ImGui::PopStyleVar();
            } else {
                menuClicked = ImGui::Button("...", ImVec2(controlSize, controlSize));
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            if (menuClicked || (headerFocused && componentContextMenuKey)) {
                ImGui::OpenPopup("ComponentMenu");
            }
            if (ImGui::BeginPopup("ComponentMenu")) {
                menuFn();
                ImGui::EndPopup();
            }
            right = menuPos.x - style.ItemSpacing.x;
        }
        if (enabled) {
            const ImVec2 checkPos(right - controlSize, headerMin.y + (headerHeight - controlSize) * 0.5f);
            ImGui::SetCursorScreenPos(checkPos);
            if (ImGui::Checkbox("##Enabled", enabled)) {
                state.enabledChanged = true;
            }
        }
        ImGui::PopID();

        if (headerIcon.id != static_cast<ImTextureID>(0)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float maxIconSize = std::max(8.0f, headerHeight - 9.0f);
            const float iconSize = (headerIcon.nativeW > 0.0f)
                                       ? std::min(headerIcon.nativeW, maxIconSize)
                                       : maxIconSize;
            const float arrowWidth = headerHeight;
            const float iconX = std::floor(headerMin.x + arrowWidth + 2.0f);
            const float iconY = std::floor(headerMin.y + (headerHeight - iconSize) * 0.5f);
            const ImVec2 uvMin = headerIcon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
            const ImVec2 uvMax = headerIcon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
            dl->AddImage(headerIcon.id,
                ImVec2(iconX, iconY),
                ImVec2(iconX + iconSize, iconY + iconSize),
                uvMin, uvMax,
                IM_COL32(255, 255, 255, 210));
        }

        ImGui::SetCursorScreenPos(cursorAfter);
        return state;
    };

    // RAII scope used inside every opened component body. Subsection foldouts push their own
    // lightweight TreeNodeEx style so component headers and nested rows stay visually separate.
    struct InspectorBodyScope {
        InspectorReveal reveal;
        explicit InspectorBodyScope(Engine& eng)
            : reveal(eng, eng.inspectorPendingRevealId, eng.inspectorPendingRevealOpen) {
            eng.inspectorPendingRevealId = 0;
            ImGui::Indent(8.0f);
        }
        bool finished = false;
        void finish() {
            if (finished) return;
            finished = true;
            ImGui::Unindent(8.0f);
            reveal.finish();
        }
        ~InspectorBodyScope() { finish(); }
        InspectorBodyScope(const InspectorBodyScope&) = delete;
        InspectorBodyScope& operator=(const InspectorBodyScope&) = delete;
    };

    enum class InspectorClipboardKind {
        None,
        UI,
        Collider,
        PlayerController,
        Rigidbody3D,
        Rigidbody2D,
        Collider2D,
        Parallax2D,
        AudioSource,
        AudioFX,
        GroundBaked,
        Obstacle,
        MapMesh,
        AIAgent,
        OffMeshLink,
        Animation,
        Skeletal,
        ReverbZone,
        Camera,
        CameraFollow2D,
        PostFX,
        ReflectionCast,
        Renderer,
        Light,
        Light2D,
        ShadowCaster2D,
        Script
    };

    struct InspectorUiClipboardData {
        UIElementComponent ui;
        std::string albedoTexturePath;
        MaterialProperties material;
    };

    struct InspectorRendererClipboardData {
        bool faceCamera = false;
        RenderType renderType = RenderType::None;
        std::string meshPath;
        int meshSourceIndex = -1;
        MaterialProperties material;
        std::string materialPath;
        std::string albedoTexturePath;
        std::string overlayTexturePath;
        std::string normalMapPath;
        std::string shaderPackPath;
        std::string vertexShaderPath;
        std::string fragmentShaderPath;
        bool useOverlay = false;
        std::vector<std::string> additionalMaterialPaths;
        UIElementComponent ui;
    };

    struct InspectorClipboard {
        InspectorClipboardKind kind = InspectorClipboardKind::None;
        InspectorUiClipboardData ui;
        ColliderComponent collider;
        PlayerControllerComponent playerController;
        RigidbodyComponent rigidbody;
        Rigidbody2DComponent rigidbody2D;
        Collider2DComponent collider2D;
        ParallaxLayer2DComponent parallax2D;
        AudioSourceComponent audioSource;
        AudioFXComponent audioFX;
        GroundBakedTypeComponent groundBaked;
        ObsticleObjectComponent obstacle;
        MapMeshComponent mapMesh;
        AIAgentComponent aiAgent;
        OffMeshLinkComponent offMeshLink;
        AnimationComponent animation;
        SkeletalAnimationComponent skeletal;
        ReverbZoneComponent reverbZone;
        CameraComponent camera;
        CameraFollow2DComponent cameraFollow2D;
        PostFXSettings postFx;
        ReflectionCastComponent reflectionCast;
        InspectorRendererClipboardData renderer;
        LightComponent light;
        Light2DComponent light2D;
        ShadowCaster2DComponent shadowCaster2D;
        ScriptComponent script;
    };

    static InspectorClipboard inspectorClipboard;

    auto markInspectorOrderChanged = [&]() {
        inspectorOrderChanged = true;
        projectManager.currentProject.hasUnsavedChanges = true;
    };

    auto drawClipboardMenus = [&](const char* copyLabel,
                                  const char* pasteNewLabel,
                                  const char* pasteOverrideLabel,
                                  bool canPaste,
                                  const auto& onCopy,
                                  const auto& onPasteNew,
                                  const auto& onPasteOverride) {
        if (ImGui::BeginMenu("Copy")) {
            if (ImGui::MenuItem(copyLabel)) {
                onCopy();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Paste")) {
            if (!canPaste) ImGui::BeginDisabled();
            if (ImGui::MenuItem(pasteNewLabel)) {
                onPasteNew();
            }
            if (ImGui::MenuItem(pasteOverrideLabel)) {
                onPasteOverride();
            }
            if (!canPaste) ImGui::EndDisabled();
            ImGui::EndMenu();
        }
    };

    auto drawReorderMenuItems = [&](const std::string& key) {
        EnsureInspectorComponentMetadata(obj);
        const auto it = std::find(obj.inspectorComponentOrder.begin(), obj.inspectorComponentOrder.end(), key);
        const bool hasEntry = it != obj.inspectorComponentOrder.end();
        const ptrdiff_t index = hasEntry ? std::distance(obj.inspectorComponentOrder.begin(), it) : -1;
        const ptrdiff_t lastIndex = static_cast<ptrdiff_t>(obj.inspectorComponentOrder.size()) - 1;

        if (ImGui::MenuItem("Find Node Reference in Scene")) {
            setPrimarySelection(obj.id, false);
        }
        if (ImGui::MenuItem("Move Component Up", nullptr, false, hasEntry && index > 0)) {
            if (MoveInspectorComponentByOffset(obj, key, -1)) {
                markInspectorOrderChanged();
            }
        }
        if (ImGui::MenuItem("Move Component Down", nullptr, false, hasEntry && index >= 0 && index < lastIndex)) {
            if (MoveInspectorComponentByOffset(obj, key, 1)) {
                markInspectorOrderChanged();
            }
        }
        if (ImGui::MenuItem("Move Component To Top", nullptr, false, hasEntry && index > 0)) {
            if (MoveInspectorComponentToEdge(obj, key, true)) {
                markInspectorOrderChanged();
            }
        }
        if (ImGui::MenuItem("Move Component To Bottom", nullptr, false, hasEntry && index >= 0 && index < lastIndex)) {
            if (MoveInspectorComponentToEdge(obj, key, false)) {
                markInspectorOrderChanged();
            }
        }
    };

    auto insertInspectorKeyAfter = [&](const std::string& anchorKey, const std::string& newKey) {
        EnsureInspectorComponentMetadata(obj);
        auto anchorIt = std::find(obj.inspectorComponentOrder.begin(), obj.inspectorComponentOrder.end(), anchorKey);
        auto existingIt = std::find(obj.inspectorComponentOrder.begin(), obj.inspectorComponentOrder.end(), newKey);
        if (existingIt != obj.inspectorComponentOrder.end()) {
            obj.inspectorComponentOrder.erase(existingIt);
        }
        if (anchorIt == obj.inspectorComponentOrder.end()) {
            obj.inspectorComponentOrder.push_back(newKey);
        } else {
            obj.inspectorComponentOrder.insert(anchorIt + 1, newKey);
        }
        markInspectorOrderChanged();
    };

    auto drawStandardComponentMenu = [&](const std::string& key,
                                         const char* copyLabel,
                                         const char* pasteNewLabel,
                                         const char* pasteOverrideLabel,
                                         bool canPaste,
                                         const auto& onReset,
                                         const auto& onCopy,
                                         const auto& onPasteNew,
                                         const auto& onPasteOverride,
                                         bool& removeFlag) {
        drawReorderMenuItems(key);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Component Values")) {
            onReset();
        }
        drawClipboardMenus(copyLabel, pasteNewLabel, pasteOverrideLabel, canPaste, onCopy, onPasteNew, onPasteOverride);
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Component")) {
            removeFlag = true;
        }
    };

    auto reloadRendererMeshAsset = [&](SceneObject& target) {
        target.meshId = -1;
        if (!target.hasRenderer || target.meshPath.empty()) {
            return;
        }

        if (target.renderType == RenderType::OBJMesh) {
            std::string err;
            target.meshId = g_objLoader.loadOBJ(target.meshPath, err);
            return;
        }

        if (target.renderType != RenderType::Model) {
            return;
        }

        ModelSceneData sceneData;
        std::string err;
        if (getModelLoader().loadModelScene(target.meshPath, sceneData, err)) {
            int sourceIndex = target.meshSourceIndex;
            if (sourceIndex < 0 || sourceIndex >= static_cast<int>(sceneData.meshIndices.size())) {
                sourceIndex = 0;
            }
            if (!sceneData.meshIndices.empty() &&
                sourceIndex >= 0 &&
                sourceIndex < static_cast<int>(sceneData.meshIndices.size())) {
                target.meshId = sceneData.meshIndices[static_cast<size_t>(sourceIndex)];
            }
            return;
        }

        ModelLoadResult result = getModelLoader().loadModel(target.meshPath);
        if (result.success) {
            target.meshId = result.meshIndex;
        }
    };

    auto assignRendererMeshAsset = [&](SceneObject& target, const fs::path& sourcePath) {
        if (sourcePath.empty() || !fs::exists(sourcePath)) {
            return false;
        }

        std::error_code ec;
        fs::directory_entry entry(sourcePath, ec);
        if (ec || !fileBrowser.isModelFile(entry)) {
            return false;
        }

        std::string ext = sourcePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        target.hasRenderer = true;
        target.meshPath = sourcePath.string();
        target.meshSourceIndex = -1;
        target.renderType = (ext == ".obj") ? RenderType::OBJMesh : RenderType::Model;
        reloadRendererMeshAsset(target);
        UpdateLegacyTypeFromComponents(target);
        return true;
    };

    auto copyRendererClipboard = [&](const SceneObject& source) {
        inspectorClipboard.kind = InspectorClipboardKind::Renderer;
        inspectorClipboard.renderer.faceCamera = source.faceCamera;
        inspectorClipboard.renderer.renderType = source.renderType;
        inspectorClipboard.renderer.meshPath = source.meshPath;
        inspectorClipboard.renderer.meshSourceIndex = source.meshSourceIndex;
        inspectorClipboard.renderer.material = source.material;
        inspectorClipboard.renderer.materialPath = source.materialPath;
        inspectorClipboard.renderer.albedoTexturePath = source.albedoTexturePath;
        inspectorClipboard.renderer.overlayTexturePath = source.overlayTexturePath;
        inspectorClipboard.renderer.normalMapPath = source.normalMapPath;
        inspectorClipboard.renderer.shaderPackPath = source.shaderPackPath;
        inspectorClipboard.renderer.vertexShaderPath = source.vertexShaderPath;
        inspectorClipboard.renderer.fragmentShaderPath = source.fragmentShaderPath;
        inspectorClipboard.renderer.useOverlay = source.useOverlay;
        inspectorClipboard.renderer.additionalMaterialPaths = source.additionalMaterialPaths;
        inspectorClipboard.renderer.ui = source.ui;
    };

    auto applyRendererClipboard = [&](SceneObject& target) {
        target.hasRenderer = true;
        target.faceCamera = inspectorClipboard.renderer.faceCamera;
        target.renderType = inspectorClipboard.renderer.renderType;
        target.meshPath = inspectorClipboard.renderer.meshPath;
        target.meshSourceIndex = inspectorClipboard.renderer.meshSourceIndex;
        target.material = inspectorClipboard.renderer.material;
        target.materialPath = inspectorClipboard.renderer.materialPath;
        target.albedoTexturePath = inspectorClipboard.renderer.albedoTexturePath;
        target.overlayTexturePath = inspectorClipboard.renderer.overlayTexturePath;
        target.normalMapPath = inspectorClipboard.renderer.normalMapPath;
        target.shaderPackPath = inspectorClipboard.renderer.shaderPackPath;
        target.vertexShaderPath = inspectorClipboard.renderer.vertexShaderPath;
        target.fragmentShaderPath = inspectorClipboard.renderer.fragmentShaderPath;
        target.useOverlay = inspectorClipboard.renderer.useOverlay;
        target.additionalMaterialPaths = inspectorClipboard.renderer.additionalMaterialPaths;
        target.ui.spriteSheetEnabled = inspectorClipboard.renderer.ui.spriteSheetEnabled;
        target.ui.spriteSheetColumns = inspectorClipboard.renderer.ui.spriteSheetColumns;
        target.ui.spriteSheetRows = inspectorClipboard.renderer.ui.spriteSheetRows;
        target.ui.spriteSheetFrame = inspectorClipboard.renderer.ui.spriteSheetFrame;
        target.ui.spriteSheetFps = inspectorClipboard.renderer.ui.spriteSheetFps;
        target.ui.spriteSheetLoop = inspectorClipboard.renderer.ui.spriteSheetLoop;
        target.ui.spriteCustomFramesEnabled = inspectorClipboard.renderer.ui.spriteCustomFramesEnabled;
        target.ui.spriteSourceWidth = inspectorClipboard.renderer.ui.spriteSourceWidth;
        target.ui.spriteSourceHeight = inspectorClipboard.renderer.ui.spriteSourceHeight;
        target.ui.spriteCustomFrames = inspectorClipboard.renderer.ui.spriteCustomFrames;
        target.ui.spriteCustomFrameNames = inspectorClipboard.renderer.ui.spriteCustomFrameNames;
        target.ui.spriteCustomFrameScales = inspectorClipboard.renderer.ui.spriteCustomFrameScales;
        target.ui.nineSliceEnabled = inspectorClipboard.renderer.ui.nineSliceEnabled;
        target.ui.nineSliceBorder = inspectorClipboard.renderer.ui.nineSliceBorder;
        target.ui.nineSliceTileEdges = inspectorClipboard.renderer.ui.nineSliceTileEdges;
        target.ui.nineSliceTileCenter = inspectorClipboard.renderer.ui.nineSliceTileCenter;
        reloadRendererMeshAsset(target);
        UpdateLegacyTypeFromComponents(target);
    };

    auto resetRendererComponent = [&](SceneObject& target) {
        const RenderType preservedType = target.renderType;
        const std::string preservedMeshPath = target.meshPath;
        const int preservedMeshSourceIndex = target.meshSourceIndex;
        const int preservedMeshId = target.meshId;
        const UIElementComponent defaultUi;

        target.hasRenderer = true;
        target.faceCamera = false;
        target.renderType = preservedType;
        target.meshPath = preservedMeshPath;
        target.meshSourceIndex = preservedMeshSourceIndex;
        target.meshId = preservedMeshId;
        target.material = MaterialProperties{};
        target.materialPath.clear();
        target.albedoTexturePath.clear();
        target.overlayTexturePath.clear();
        target.normalMapPath.clear();
        target.shaderPackPath.clear();
        target.vertexShaderPath.clear();
        target.fragmentShaderPath.clear();
        target.useOverlay = false;
        target.additionalMaterialPaths.clear();
        target.ui.spriteSheetEnabled = defaultUi.spriteSheetEnabled;
        target.ui.spriteSheetColumns = defaultUi.spriteSheetColumns;
        target.ui.spriteSheetRows = defaultUi.spriteSheetRows;
        target.ui.spriteSheetFrame = defaultUi.spriteSheetFrame;
        target.ui.spriteSheetFps = defaultUi.spriteSheetFps;
        target.ui.spriteSheetLoop = defaultUi.spriteSheetLoop;
        target.ui.spriteCustomFramesEnabled = defaultUi.spriteCustomFramesEnabled;
        target.ui.spriteSourceWidth = defaultUi.spriteSourceWidth;
        target.ui.spriteSourceHeight = defaultUi.spriteSourceHeight;
        target.ui.spriteCustomFrames.clear();
        target.ui.spriteCustomFrameNames.clear();
        target.ui.spriteCustomFrameScales.clear();
        target.ui.nineSliceEnabled = defaultUi.nineSliceEnabled;
        target.ui.nineSliceBorder = defaultUi.nineSliceBorder;
        target.ui.nineSliceTileEdges = defaultUi.nineSliceTileEdges;
        target.ui.nineSliceTileCenter = defaultUi.nineSliceTileCenter;

        if (preservedType == RenderType::Sprite) {
            target.material.ambientStrength = 1.0f;
        } else if (preservedType == RenderType::Mirror) {
            target.useOverlay = true;
            target.material.textureMix = 1.0f;
            target.material.color = glm::vec3(1.0f);
        }

        UpdateLegacyTypeFromComponents(target);
    };

    const bool runtimeSceneEditingLocked = isPlaying || specMode || testMode || playerMode;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));

    char nameBuffer[128];
    strncpy(nameBuffer, obj.name.c_str(), sizeof(nameBuffer));
    nameBuffer[sizeof(nameBuffer) - 1] = '\0';

    bool nameHovered = false;

    if (ImGui::BeginTable("##ObjectMetaTopTable", 4, ImGuiTableFlags_SizingStretchProp))
    {
        const float objIconSize = ImGui::GetFrameHeight() + 4.0f;
        const ImGuiStyle& metaStyle = ImGui::GetStyle();
        const float checkboxWidth = metaStyle.CheckboxSwitch
            ? std::floor(ImGui::GetFrameHeight() * 1.70f)
            : ImGui::GetFrameHeight();
        const float invariableColWidth = checkboxWidth + 2.0f;
        ImGui::TableSetupColumn("IconColumn",      ImGuiTableColumnFlags_WidthFixed, objIconSize);
        ImGui::TableSetupColumn("EnableColumn",    ImGuiTableColumnFlags_WidthFixed, checkboxWidth + 2.0f);
        ImGui::TableSetupColumn("NameColumn",      ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("InvariableColumn",ImGuiTableColumnFlags_WidthFixed, invariableColWidth);

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        const std::string tintPopupId = std::string("##HierarchyTintPicker_") + std::to_string(obj.id);
        const InspectorUiIcon customObjectIcon = obj.editorIconPath.empty()
            ? InspectorUiIcon{}
            : resolveInspectorIcon(obj.editorIconPath.c_str(),
                                   MaterialProperties::TextureFilter::Bilinear);
        const InspectorUiIcon displayObjectIcon =
            customObjectIcon.id != static_cast<ImTextureID>(0) ? customObjectIcon : iconGameObject;
        if (displayObjectIcon.id != static_cast<ImTextureID>(0)) {
            const ImVec2 uvMin = displayObjectIcon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
            const ImVec2 uvMax = displayObjectIcon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
            const ImVec2 iconMin = ImGui::GetCursorScreenPos();
            const ImVec2 iconMax(iconMin.x + objIconSize, iconMin.y + objIconSize);
            const ImVec4 iconTint(obj.editorIconTint.r, obj.editorIconTint.g,
                                  obj.editorIconTint.b, obj.editorIconTint.a);
            ImGui::GetWindowDrawList()->AddImage(displayObjectIcon.id, iconMin, iconMax, uvMin, uvMax,
                ImGui::ColorConvertFloat4ToU32(iconTint));
            if (ImGui::InvisibleButton("##HierarchyIconTintBtn", ImVec2(objIconSize, objIconSize))) {
                ImGui::OpenPopup(tintPopupId.c_str());
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Click to set the object icon and color");
            if (ImGui::BeginPopup(tintPopupId.c_str())) {
                ImGui::TextDisabled("Object Icon");
                ImGui::Separator();
                ImGui::ColorPicker4("##HierarchyTintColor", &obj.editorIconTint.r,
                    ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoAlpha |
                    ImGuiColorEditFlags_PickerHueWheel);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    objectIconChanged = true;
                    projectManager.currentProject.hasUnsavedChanges = true;
                }

                ImGui::Separator();
                ImGui::TextDisabled("Custom Image");
                const std::string currentIconName = obj.editorIconPath.empty()
                    ? std::string("Default icon")
                    : assetDisplayName(obj.editorIconPath, "Custom icon");
                ImGui::TextUnformatted(currentIconName.c_str());
                if (!obj.editorIconPath.empty() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", obj.editorIconPath.c_str());
                }

                const bool selectionIsImage = isTextureOrSpriteSheetSelection(fileBrowser.selectedFile);
                ImGui::BeginDisabled(!selectionIsImage);
                if (ImGui::Button("Use Selected Image", ImVec2(-1.0f, 0.0f))) {
                    obj.editorIconPath = fileBrowser.selectedFile.string();
                    objectIconChanged = true;
                    projectManager.currentProject.hasUnsavedChanges = true;
                }
                ImGui::EndDisabled();
                if (!selectionIsImage && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Select an image in the Project browser first.");
                }

                static char iconFilterBuf[64] = "";
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##IconAssetFilter", "Filter images...",
                                         iconFilterBuf, sizeof(iconFilterBuf));
                ImGui::BeginChild("##IconAssetList", ImVec2(260.0f, 150.0f), false);
                for (const fs::path& assetPath : collectProjectTextureAssets()) {
                    const std::string assetName = assetDisplayName(assetPath.string(), "Image");
                    if (!matchesPopupFilter(assetName, iconFilterBuf)) {
                        continue;
                    }
                    if (ImGui::Selectable(assetName.c_str(), assetPath.string() == obj.editorIconPath)) {
                        obj.editorIconPath = assetPath.string();
                        objectIconChanged = true;
                        projectManager.currentProject.hasUnsavedChanges = true;
                    }
                }
                ImGui::EndChild();

                if (ImGui::Checkbox("Show In Viewport", &obj.editorIconShowInViewport)) {
                    objectIconChanged = true;
                    projectManager.currentProject.hasUnsavedChanges = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Billboard this icon in the scene viewport at the object's origin.");
                }

                ImGui::Separator();
                if (ImGui::Button("Reset to Default", ImVec2(-1.0f, 0.0f))) {
                    obj.editorIconTint = glm::vec4(1.0f);
                    obj.editorIconPath.clear();
                    obj.editorIconShowInViewport = false;
                    objectIconChanged = true;
                    projectManager.currentProject.hasUnsavedChanges = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        } else {
            ImGui::Dummy(ImVec2(objIconSize, objIconSize));
        }

        ImGui::TableSetColumnIndex(1);
        if (mixedCheckbox("##Enabled", &obj.enabled,
                          [](const SceneObject& selectedObj) { return selectedObj.enabled; },
                          [&](bool value) { obj.enabled = value; }))
        {
            objectEnabledChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }

        ImGui::TableSetColumnIndex(2);
        ImGui::BeginDisabled(runtimeSceneEditingLocked);
        ImGui::SetNextItemWidth(-1);
        const std::string oldNameBeforeEdit = obj.name;
        if (mixedInputText("##Name", nameBuffer, sizeof(nameBuffer),
                           [](const SceneObject& selectedObj) { return selectedObj.name; },
                           [&](const std::string& value) { obj.name = value; }))
        {
            const std::string newName = nameBuffer;
            if (oldNameBeforeEdit != newName)
            {
                objectNameChanged = true;
                propagateObjectRenameReferences(oldNameBeforeEdit, newName, obj.id);
                projectManager.currentProject.hasUnsavedChanges = true;
            }
        }
        nameHovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(3);
        if (iconInvariableOff.id != static_cast<ImTextureID>(0) ||
            iconInvariableOn.id != static_cast<ImTextureID>(0))
        {
            const bool flipY = iconInvariableOff.id != static_cast<ImTextureID>(0)
                ? iconInvariableOff.flipY
                : iconInvariableOn.flipY;
            ImGui::SetNextItemCheckboxIcons(iconInvariableOff.id, iconInvariableOn.id,
                flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f),
                flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f));
        }
        if (mixedCheckbox("##Invariable", &obj.IsInvariable,
                          [](const SceneObject& selectedObj) { return selectedObj.IsInvariable; },
                          [&](bool value) { obj.IsInvariable = value; }))
        {
            objectInvariableChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextUnformatted("Invariable objects are locked from physical transform changes and direct scene manipulation. Scripts may still reference them, and material-based properties can still update.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        ImGui::EndTable();
    }

    if (runtimeSceneEditingLocked && nameHovered)
    {
        ImGui::SetTooltip("Object renaming is disabled while the scene is running.");
    }

    char tagBuf[64] = {};
    std::snprintf(tagBuf, sizeof(tagBuf), "%s", obj.tag.c_str());

    if (ImGui::BeginTable("##ObjectMetaBottomTable", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("TagColumn", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("LayerColumn", ImGuiTableColumnFlags_WidthStretch, 1.0f);

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Tag");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (mixedInputText("##Tag", tagBuf, sizeof(tagBuf),
                           [](const SceneObject& selectedObj) { return selectedObj.tag; },
                           [&](const std::string& value) { obj.tag = value; },
                           "Untagged"))
        {
            objectTagChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Layer");
        ImGui::SameLine();
        int layer = obj.layer;
        ImGui::SetNextItemWidth(-1);
        if (mixedSliderInt("##Layer", &layer, 0, 31, "%d",
                           [](const SceneObject& selectedObj) { return selectedObj.layer; },
                           [&](int value) { obj.layer = value; }))
        {
            objectLayerChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }

        ImGui::EndTable();
        /*{
            const float iconSz = ImGui::GetFrameHeight();
            if (iconTransform.id != static_cast<ImTextureID>(0)) {
                const ImVec2 uvMin = iconTransform.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
                const ImVec2 uvMax = iconTransform.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
                ImGui::Image(iconTransform.id, ImVec2(iconSz, iconSz), uvMin, uvMax);
                ImGui::SameLine(0.0f, 4.0f);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Transform");
            ImGui::Separator();
        }*/

        const bool editingLocalTransform = (obj.parentId != -1) && obj.localInitialized;
        glm::vec3& editPosition = editingLocalTransform ? obj.localPosition : obj.position;
        glm::vec3& editRotation = editingLocalTransform ? obj.localRotation : obj.rotation;
        glm::vec3& editScale = editingLocalTransform ? obj.localScale : obj.scale;

        auto commitTransformEdit = [&]() {
            if (editingLocalTransform) {
                obj.localInitialized = true;
                updateHierarchyWorldTransforms();
            } else {
                syncLocalTransform(obj);
            }
            objectTransformChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        };

        auto transformFieldGetter = [](glm::vec3 SceneObject::*localField,
                                       glm::vec3 SceneObject::*worldField) {
            return [localField, worldField](const SceneObject& selectedObj, int component) {
                const bool local = (selectedObj.parentId != -1) && selectedObj.localInitialized;
                const glm::vec3& value = local ? selectedObj.*localField : selectedObj.*worldField;
                return (&value.x)[component];
            };
        };

        const SceneObject* transformParent =
            editingLocalTransform ? findObjectById(obj.parentId) : nullptr;
        auto transformRowLabel = [&](const char* text) {
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", text);
            if (transformParent && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Relative to parent: %s", transformParent->name.c_str());
            }
        };

        if (obj.IsInvariable) {
            ImGui::TextDisabled("Transform locked by Invariable");
        }
        ImGui::BeginDisabled(obj.IsInvariable);

        if (ImGui::BeginTable("##TransformTable", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("LabelColumn", ImGuiTableColumnFlags_WidthFixed, 62.0f);
            ImGui::TableSetupColumn("ValueColumn", ImGuiTableColumnFlags_WidthStretch, 1.0f);

            ImGui::TableNextRow();
            transformRowLabel(Loc::Field("COMPONENT_TRANSFORM", "position", "Position"));

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (mixedDragFloatN("##Position", &editPosition.x, 3, 0.1f, 0.0f, 0.0f, "%.3f",
                                transformFieldGetter(&SceneObject::localPosition,
                                                     &SceneObject::position),
                                [&](int component, float value) {
                                    (&editPosition.x)[component] = value;
                                }))
            {
                commitTransformEdit();
            }

            ImGui::TableNextRow();
            transformRowLabel(Loc::Field("COMPONENT_TRANSFORM", "rotation", "Rotation"));

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (mixedDragFloatN("##Rotation", &editRotation.x, 3, 1.0f, -360.0f, 360.0f, "%.3f",
                                transformFieldGetter(&SceneObject::localRotation,
                                                     &SceneObject::rotation),
                                [&](int component, float value) {
                                    (&editRotation.x)[component] = value;
                                }))
            {
                editRotation = NormalizeEulerDegrees(editRotation);
                commitTransformEdit();
            }

            ImGui::TableNextRow();
            transformRowLabel(Loc::Field("COMPONENT_TRANSFORM", "scale", "Scale"));

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (mixedDragFloatN("##Scale", &editScale.x, 3, 0.05f, 0.01f, 100.0f, "%.3f",
                                transformFieldGetter(&SceneObject::localScale,
                                                     &SceneObject::scale),
                                [&](int component, float value) {
                                    (&editScale.x)[component] = value;
                                },
                                1.0f))
            {
                commitTransformEdit();
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
    }

    ImGui::PopStyleVar(2);

    // Inspector field layout helpers
    auto beginCompFields = [](const char* id) -> bool {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
            return false;
        ImGui::TableSetupColumn("##L", ImGuiTableColumnFlags_WidthStretch, 40.0f);
        ImGui::TableSetupColumn("##V", ImGuiTableColumnFlags_WidthStretch, 60.0f);
        return true;
    };
    auto endCompFields = []() { ImGui::EndTable(); };
    auto fieldRow = [](const char* label) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1.0f);
    };
    // Unity-style: label in col 0, checkbox left-aligned in col 1.
    auto boolRow = [](const char* label, bool* val) -> bool {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(static_cast<const void*>(val));
        bool r = ImGui::Checkbox("##chk", val);
        ImGui::PopID();
        return r;
    };

    auto HorizontalBoolRow = [](const char* label,
                                const char* labelA, bool* valA,
                                const char* labelB, bool* valB) -> bool
    {
        bool changed = false;

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);

        ImGui::TableSetColumnIndex(1);

        ImGui::PushID(label);

        if (ImGui::Checkbox(labelA, valA))
            changed = true;

        ImGui::SameLine();

        if (ImGui::Checkbox(labelB, valB))
            changed = true;

        ImGui::PopID();

        return changed;
    };

    // Note/hint text indented under the value column, wraps within its cell width.
    auto noteRow = [](const char* text) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
    };

    const std::vector<std::string> inspectorComponentOrder = obj.inspectorComponentOrder;
    for (const std::string& inspectorComponentKey : inspectorComponentOrder) {
    if (inspectorComponentKey == "ui" && isUIObject(obj) && sharedUIObject) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.45f, 0.65f, 1.0f));
        bool changed = false;
        bool removeUi = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_UI", "UI"), "UI", "ui", nullptr, true, [&]() {
            drawStandardComponentMenu(
                "ui",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::UI,
                [&]() {
                    const UIElementType type = obj.ui.type;
                    obj.ui = UIElementComponent{};
                    obj.ui.type = type;
                    obj.albedoTexturePath.clear();
                    obj.material = MaterialProperties{};
                    changed = true;
                },
                [&]() {
                    inspectorClipboard.kind = InspectorClipboardKind::UI;
                    inspectorClipboard.ui.ui = obj.ui;
                    inspectorClipboard.ui.albedoTexturePath = obj.albedoTexturePath;
                    inspectorClipboard.ui.material = obj.material;
                },
                [&]() {
                    obj.hasUI = true;
                    obj.ui = inspectorClipboard.ui.ui;
                    obj.albedoTexturePath = inspectorClipboard.ui.albedoTexturePath;
                    obj.material = inspectorClipboard.ui.material;
                    changed = true;
                },
                [&]() {
                    obj.ui = inspectorClipboard.ui.ui;
                    obj.albedoTexturePath = inspectorClipboard.ui.albedoTexturePath;
                    obj.material = inspectorClipboard.ui.material;
                    changed = true;
                },
                removeUi);
        });
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("UI");
            UIControls::InspectorHooks uiHooks;
            uiHooks.section = [&](const char* label, bool defaultOpen, const std::function<void()>& body) {
                if (InspectorReveal reveal = drawInspectorSubsectionFoldout(label, nullptr, defaultOpen)) body();
            };
            uiHooks.objectSlot = [&](const char* id, const char* label, int& objectId, const char* typeName) {
                return drawObjectReferenceSlot(id, label, objectId, obj.id, typeName);
            };
            changed |= UIControls::Inspector(obj.ui, obj.IsInvariable, uiHooks);

            if (obj.IsInvariable) {
                ImGui::TextDisabled("UI transform locked by Invariable");
            }
            ImGui::BeginDisabled(obj.IsInvariable);

            if (obj.type != ObjectType::Sprite25D) {
                const char* anchors[] = { "Center", "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
                int anchor = static_cast<int>(obj.ui.anchor);
                if (mixedCombo("Anchor", &anchor, anchors, IM_ARRAYSIZE(anchors),
                               [](const SceneObject& selectedObj) { return static_cast<int>(selectedObj.ui.anchor); },
                               [&](int value) { obj.ui.anchor = static_cast<UIAnchor>(value); })) {
                    obj.ui.anchor = static_cast<UIAnchor>(anchor);
                    changed = true;
                }

                if (mixedDragFloatN(Loc::Widget("COMPONENT_UI_POSITION_PX", "Position (px)"), &obj.ui.position.x, 2, 1.0f, 0.0f, 0.0f, "%.3f",
                                    [](const SceneObject& selectedObj, int component) {
                                        return (&selectedObj.ui.position.x)[component];
                                    },
                                    [&](int component, float value) {
                                        (&obj.ui.position.x)[component] = value;
                                    })) {
                    changed = true;
                }
            } else {
                ImGui::TextDisabled("Anchor and UI position are ignored for projected 2.5D sprites.");
            }

            if (mixedDragFloat(Loc::Widget("COMPONENT_UI_ROTATION_DEG", "Rotation (deg)"), &obj.ui.rotation, 0.5f, -360.0f, 360.0f, "%.3f",
                               [](const SceneObject& selectedObj) { return selectedObj.ui.rotation; },
                               [&](float value) { obj.ui.rotation = value; })) {
                glm::vec3 rot(0.0f, 0.0f, obj.ui.rotation);
                rot = NormalizeEulerDegrees(rot);
                obj.ui.rotation = rot.z;
                changed = true;
            }

            glm::vec2 minSize(1.0f, 1.0f);
            if (obj.ui.type == UIElementType::Image || obj.ui.type == UIElementType::Sprite2D) {
                minSize = glm::vec2(0.01f, 0.01f);
            }
            if (mixedDragFloatN(Loc::Widget("COMPONENT_UI_SIZE_PX", "Size (px)"), &obj.ui.size.x, 2, 1.0f, minSize.x, 65536.0f, "%.3f",
                                [](const SceneObject& selectedObj, int component) {
                                    return (&selectedObj.ui.size.x)[component];
                                },
                                [&](int component, float value) {
                                    (&obj.ui.size.x)[component] = value;
                                },
                                minSize.x)) {
                obj.ui.size.x = std::max(minSize.x, obj.ui.size.x);
                obj.ui.size.y = std::max(minSize.y, obj.ui.size.y);
                changed = true;
            }
            ImGui::EndDisabled();

            if (obj.ui.type == UIElementType::Canvas) {
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_RENDER_IN_3_D", "Render In 3D"), &obj.ui.renderIn3D)) {
                    changed = true;
                }
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_MASK_CHILDREN", "Mask Children"), &obj.ui.maskChildren)) {
                    changed = true;
                }
                if (obj.ui.renderIn3D) {
                    if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_FACE_CAMERA", "Face Camera"), &obj.faceCamera)) {
                        changed = true;
                    }
                    int size[2] = { obj.ui.renderTargetSize.x, obj.ui.renderTargetSize.y };
                    if (ImGui::DragInt2("Render Target (px)", size, 1.0f, 16, 4096)) {
                        obj.ui.renderTargetSize.x = std::max(16, size[0]);
                        obj.ui.renderTargetSize.y = std::max(16, size[1]);
                        changed = true;
                    }
                    const Renderer::UiTargetInfo liveTarget = renderer.getUiTargetInfo(obj.id);
                    if (liveTarget.texture != 0 &&
                        (liveTarget.width != obj.ui.renderTargetSize.x ||
                         liveTarget.height != obj.ui.renderTargetSize.y)) {
                        ImGui::TextDisabled("Rendering at %d x %d (Project Settings > 3D Canvas)",
                                            liveTarget.width, liveTarget.height);
                    }
                    const char* renderFilterOptions[] = { "Bilinear", "No Filter" };
                    int renderFilter = (obj.ui.renderTargetFilter == MaterialProperties::TextureFilter::Point) ? 1 : 0;
                    if (ImGui::Combo(Loc::Widget("COMPONENT_UI_RENDER_FILTER", "Render Filter"), &renderFilter, renderFilterOptions, IM_ARRAYSIZE(renderFilterOptions))) {
                        obj.ui.renderTargetFilter = (renderFilter == 1)
                            ? MaterialProperties::TextureFilter::Point
                            : MaterialProperties::TextureFilter::Bilinear;
                        changed = true;
                    }
                    ImGui::TextDisabled("Canvas renders on a 3D quad; use object scale for world size.");
                } else {
                    if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_PSEUDO_3_D_ENABLED", "Pseudo 3D Enabled"), &obj.ui.pseudo3DEnabled)) {
                        changed = true;
                    }
                    if (obj.ui.pseudo3DEnabled) {
                        if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_USE_OFFSCREEN_SURFACE", "Use Offscreen Surface"), &obj.ui.pseudo3DUseOffscreenSurface)) {
                            changed = true;
                        }

                        if (ImGui::DragFloat2("Pseudo Panel Size", &obj.ui.pseudo3DPanelSize.x, 1.0f, 0.0f, 4096.0f)) {
                            obj.ui.pseudo3DPanelSize.x = std::max(0.0f, obj.ui.pseudo3DPanelSize.x);
                            obj.ui.pseudo3DPanelSize.y = std::max(0.0f, obj.ui.pseudo3DPanelSize.y);
                            changed = true;
                        }
                        int pseudoRt[2] = { obj.ui.renderTargetSize.x, obj.ui.renderTargetSize.y };
                        if (ImGui::DragInt2("Pseudo RT (px)", pseudoRt, 1.0f, 16, 4096)) {
                            obj.ui.renderTargetSize.x = std::max(16, pseudoRt[0]);
                            obj.ui.renderTargetSize.y = std::max(16, pseudoRt[1]);
                            changed = true;
                        }

                        if (ImGui::DragFloat2("Top Left Offset", &obj.ui.pseudo3DTopLeftOffset.x, 0.25f, -4096.0f, 4096.0f)) changed = true;
                        if (ImGui::DragFloat2("Top Right Offset", &obj.ui.pseudo3DTopRightOffset.x, 0.25f, -4096.0f, 4096.0f)) changed = true;
                        if (ImGui::DragFloat2("Bottom Right Offset", &obj.ui.pseudo3DBottomRightOffset.x, 0.25f, -4096.0f, 4096.0f)) changed = true;
                        if (ImGui::DragFloat2("Bottom Left Offset", &obj.ui.pseudo3DBottomLeftOffset.x, 0.25f, -4096.0f, 4096.0f)) changed = true;

                        if (ImGui::SliderFloat2("Pseudo Pivot", &obj.ui.pseudo3DPivot.x, 0.0f, 1.0f, "%.2f")) {
                            obj.ui.pseudo3DPivot.x = std::clamp(obj.ui.pseudo3DPivot.x, 0.0f, 1.0f);
                            obj.ui.pseudo3DPivot.y = std::clamp(obj.ui.pseudo3DPivot.y, 0.0f, 1.0f);
                            changed = true;
                        }

                        if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_PERSPECTIVE_INTENSITY", "Perspective Intensity"), &obj.ui.pseudo3DPerspectiveIntensity, 0.01f, -2.0f, 2.0f, "%.2f")) changed = true;
                        if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_SKEW_AMOUNT", "Skew Amount"), &obj.ui.pseudo3DSkewAmount, 0.01f, -2.0f, 2.0f, "%.2f")) changed = true;
                        if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_CURVATURE_AMOUNT", "Curvature Amount"), &obj.ui.pseudo3DCurvatureAmount, 0.01f, -2.0f, 2.0f, "%.2f")) changed = true;

                        std::string anchorLabel = "None";
                        if (obj.ui.pseudo3DAnchorTargetId >= 0) {
                            if (const SceneObject* anchorObj = findObjectById(obj.ui.pseudo3DAnchorTargetId)) {
                                anchorLabel = anchorObj->name + " (" + std::to_string(anchorObj->id) + ")";
                            }
                        }
                        if (ImGui::BeginCombo("Anchor Target", anchorLabel.c_str())) {
                            if (ImGui::Selectable("None", obj.ui.pseudo3DAnchorTargetId < 0)) {
                                obj.ui.pseudo3DAnchorTargetId = -1;
                                changed = true;
                            }
                            for (const auto& candidate : sceneObjects) {
                                if (candidate.id == obj.id) continue;
                                const std::string label = candidate.name + " (" + std::to_string(candidate.id) + ")";
                                const bool selected = (candidate.id == obj.ui.pseudo3DAnchorTargetId);
                                if (ImGui::Selectable(label.c_str(), selected)) {
                                    obj.ui.pseudo3DAnchorTargetId = candidate.id;
                                    changed = true;
                                }
                                if (selected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }

                        if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_DISTANCE_SCALING_ENABLED", "Distance Scaling Enabled"), &obj.ui.pseudo3DDistanceScalingEnabled)) changed = true;
                        if (obj.ui.pseudo3DDistanceScalingEnabled) {
                            if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_MIN_DISTANCE", "Min Distance"), &obj.ui.pseudo3DMinDistance, 0.05f, 0.01f, 10000.0f, "%.2f")) {
                                obj.ui.pseudo3DMinDistance = std::max(0.01f, obj.ui.pseudo3DMinDistance);
                                changed = true;
                            }
                            if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_MAX_DISTANCE", "Max Distance"), &obj.ui.pseudo3DMaxDistance, 0.05f, 0.02f, 10000.0f, "%.2f")) {
                                obj.ui.pseudo3DMaxDistance = std::max(obj.ui.pseudo3DMinDistance + 0.01f, obj.ui.pseudo3DMaxDistance);
                                changed = true;
                            }
                            if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_PERSPECTIVE_SCALES_WITH_DISTANCE", "Perspective Scales With Distance"), &obj.ui.pseudo3DAdjustPerspectiveWithDistance)) changed = true;
                        }

                        if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_INTERACTION_DISTANCE", "Interaction Distance"), &obj.ui.pseudo3DInteractionDistance, 0.05f, 0.0f, 10000.0f, "%.2f")) {
                            obj.ui.pseudo3DInteractionDistance = std::max(0.0f, obj.ui.pseudo3DInteractionDistance);
                            changed = true;
                        }
                        ImGui::TextDisabled("Interaction Distance = 0 disables distance gating.");
                        if (ImGui::DragInt("Depth Sort / Draw Order", &obj.ui.pseudo3DDepthSort, 1.0f, -2048, 2048)) changed = true;
                        if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_ALLOW_INTERACTION", "Allow Interaction"), &obj.ui.pseudo3DAllowInteraction)) changed = true;
                    }
                }
            }

            if (obj.ui.type == UIElementType::Button || obj.ui.type == UIElementType::Slider) {
                if (mixedCheckbox("Interactable", &obj.ui.interactable,
                                  [](const SceneObject& selectedObj) { return selectedObj.ui.interactable; },
                                  [&](bool value) { obj.ui.interactable = value; })) {
                    changed = true;
                }

                const auto& presets = getUIStylePresets();
                if (!presets.empty()) {
                    int presetIndex = findUIStylePreset(obj.ui.stylePreset);
                    if (presetIndex < 0) presetIndex = 0;
                    const char* currentPreset = presets[presetIndex].name.c_str();
                    if (ImGui::BeginCombo("Style Preset", currentPreset)) {
                        for (int i = 0; i < (int)presets.size(); ++i) {
                            bool selected = (i == presetIndex);
                            if (ImGui::Selectable(presets[i].name.c_str(), selected)) {
                                obj.ui.stylePreset = presets[i].name;
                                changed = true;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
            }

            if (obj.ui.type == UIElementType::Button || obj.ui.type == UIElementType::Slider ||
                obj.ui.type == UIElementType::Image || obj.ui.type == UIElementType::Text ||
                obj.ui.type == UIElementType::Sprite2D) {
                if (obj.ui.type == UIElementType::Text) {
                    char labelBuf[4096] = {};
                    std::snprintf(labelBuf, sizeof(labelBuf), "%s", obj.ui.label.c_str());
                    if (selectedValuesMixed([](const SceneObject& selectedObj) { return selectedObj.ui.label; })) {
                        if (drawMixedValuePopup("Text", obj.ui.label,
                                                [](const SceneObject& selectedObj) { return selectedObj.ui.label; },
                                                [&](const std::string& value) { obj.ui.label = value; },
                                                [&]() { obj.ui.label.clear(); })) {
                            changed = true;
                        }
                    } else if (ImGui::InputTextMultiline("Text", labelBuf, sizeof(labelBuf), ImVec2(-FLT_MIN, 96.0f))) {
                        obj.ui.label = labelBuf;
                        changed = true;
                    }
                } else {
                    char labelBuf[128] = {};
                    std::snprintf(labelBuf, sizeof(labelBuf), "%s", obj.ui.label.c_str());
                    if (mixedInputText("Label", labelBuf, sizeof(labelBuf),
                                       [](const SceneObject& selectedObj) { return selectedObj.ui.label; },
                                       [&](const std::string& value) { obj.ui.label = value; })) {
                        changed = true;
                    }
                }
            }
            if (obj.ui.type == UIElementType::Text) {
                const float baseTextSize = std::max(1.0f, ImGui::GetFontSize());
                float textSizePx = (obj.ui.fontSize > 0.0f)
                    ? obj.ui.fontSize
                    : baseTextSize * std::max(0.1f, obj.ui.textScale);
                if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_TEXT_SIZE_PX", "Text Size (px)"), &textSizePx, 0.5f, 1.0f, 512.0f, "%.1f")) {
                    obj.ui.fontSize = std::max(1.0f, textSizePx);
                    obj.ui.textScale = obj.ui.fontSize / baseTextSize;
                    changed = true;
                }
                if (obj.ui.fontSize > 0.0f) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset##UITextFontSize")) {
                        obj.ui.fontSize = 0.0f;
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Use the legacy Text Size multiplier instead of an explicit pixel size.");
                    }
                }
                const auto& fontCatalog = getUIFontCatalog();
                std::string currentFontLabel = "Use Editor Style Font";
                const int currentFontIndex = findUIFontCatalogIndex(obj.ui.textFont);
                if (!obj.ui.textFont.empty()) {
                    if (currentFontIndex >= 0) {
                        currentFontLabel = fontCatalog[static_cast<size_t>(currentFontIndex)].label;
                    } else {
                        currentFontLabel = obj.ui.textFont;
                    }
                }
                if (ImGui::BeginCombo("Text Font", currentFontLabel.c_str())) {
                    const bool useEditorFont = obj.ui.textFont.empty();
                    if (ImGui::Selectable("Use Editor Style Font", useEditorFont)) {
                        obj.ui.textFont.clear();
                        changed = true;
                    }
                    if (useEditorFont) ImGui::SetItemDefaultFocus();
                    for (size_t i = 0; i < fontCatalog.size(); ++i) {
                        const bool selected = obj.ui.textFont == fontCatalog[i].id;
                        if (ImGui::Selectable(fontCatalog[i].label.c_str(), selected)) {
                            obj.ui.textFont = fontCatalog[i].id;
                            changed = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_AUTO_WRAP", "Auto Wrap"), &obj.ui.textAutoWrap)) {
                    changed = true;
                }
                const char* hAlignLabels[] = { "Left", "Center", "Right" };
                int hAlignIndex = static_cast<int>(obj.ui.textHAlign);
                if (ImGui::Combo(Loc::Widget("COMPONENT_UI_HORIZONTAL_ALIGN", "Horizontal Align"), &hAlignIndex, hAlignLabels, IM_ARRAYSIZE(hAlignLabels))) {
                    obj.ui.textHAlign = static_cast<UITextHAlign>(std::clamp(hAlignIndex, 0, 2));
                    changed = true;
                }
                const char* vAlignLabels[] = { "Top", "Middle", "Bottom" };
                int vAlignIndex = static_cast<int>(obj.ui.textVAlign);
                if (ImGui::Combo(Loc::Widget("COMPONENT_UI_VERTICAL_ALIGN", "Vertical Align"), &vAlignIndex, vAlignLabels, IM_ARRAYSIZE(vAlignLabels))) {
                    obj.ui.textVAlign = static_cast<UITextVAlign>(std::clamp(vAlignIndex, 0, 2));
                    changed = true;
                }
                if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_EFFECT_SPEED", "Effect Speed"), &obj.ui.textEffectSpeed, 0.01f, 0.01f, 20.0f, "%.2f")) {
                    obj.ui.textEffectSpeed = std::max(0.01f, obj.ui.textEffectSpeed);
                    changed = true;
                }
                if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_EFFECT_INTENSITY", "Effect Intensity"), &obj.ui.textEffectIntensity, 0.01f, 0.0f, 10.0f, "%.2f")) {
                    obj.ui.textEffectIntensity = std::max(0.0f, obj.ui.textEffectIntensity);
                    changed = true;
                }
                int textEffectFlags = obj.ui.textEffectFlags;
                bool wave = (textEffectFlags & (1 << 0)) != 0;
                bool shake = (textEffectFlags & (1 << 1)) != 0;
                bool bounce = (textEffectFlags & (1 << 2)) != 0;
                bool rotate = (textEffectFlags & (1 << 3)) != 0;
                bool fade = (textEffectFlags & (1 << 4)) != 0;
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_WAVE", "Wave"), &wave)) {
                    if (wave) textEffectFlags |= (1 << 0);
                    else textEffectFlags &= ~(1 << 0);
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_SHAKE", "Shake"), &shake)) {
                    if (shake) textEffectFlags |= (1 << 1);
                    else textEffectFlags &= ~(1 << 1);
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_BOUNCE", "Bounce"), &bounce)) {
                    if (bounce) textEffectFlags |= (1 << 2);
                    else textEffectFlags &= ~(1 << 2);
                    changed = true;
                }
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_ROTATE", "Rotate"), &rotate)) {
                    if (rotate) textEffectFlags |= (1 << 3);
                    else textEffectFlags &= ~(1 << 3);
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_FADE", "Fade"), &fade)) {
                    if (fade) textEffectFlags |= (1 << 4);
                    else textEffectFlags &= ~(1 << 4);
                    changed = true;
                }
                obj.ui.textEffectFlags = textEffectFlags;
                const char* textFilterOptions[] = { "Bilinear", "Point" };
                int textFilterIndex = (obj.material.textureFilter == MaterialProperties::TextureFilter::Point) ? 1 : 0;
                if (ImGui::Combo(Loc::Widget("COMPONENT_UI_TEXT_FILTER", "Text Filter"), &textFilterIndex, textFilterOptions, IM_ARRAYSIZE(textFilterOptions))) {
                    obj.material.textureFilter =
                        (textFilterIndex == 1) ? MaterialProperties::TextureFilter::Point
                                               : MaterialProperties::TextureFilter::Bilinear;
                    changed = true;
                }
            }

            if (obj.ui.type == UIElementType::Image || obj.ui.type == UIElementType::Sprite2D) {
                // Assigning a sprite also pulls in the clips from its .spritesheet sidecar,
                // so the slot routes through assignSpriteTextureOrClips instead of just
                // writing the path.
                fs::path assignedSprite;
                if (drawAssetReferenceSlot("##UITexture", "Texture", obj.albedoTexturePath,
                                           FileCategory::Texture, "Sprite", assignedSprite)) {
                    if (assignedSprite.empty()) {
                        obj.albedoTexturePath.clear();
                        obj.ui.spriteCustomFrames.clear();
                        obj.ui.spriteCustomFrameNames.clear();
                        obj.ui.spriteCustomFramesEnabled = false;
                        obj.ui.spriteSourceWidth = 0;
                        obj.ui.spriteSourceHeight = 0;
                        obj.ui.spriteSheetFrame = 0;
                        changed = true;
                    } else if (assignSpriteTextureOrClips(obj, assignedSprite)) {
                        changed = true;
                    }
                }
                ImGui::BeginDisabled(obj.albedoTexturePath.empty());
                if (ImGui::SmallButton("Reload Clips##UITexture")) {
                    if (assignSpriteTextureOrClips(obj, fs::path(obj.albedoTexturePath))) {
                        changed = true;
                    }
                }
                ImGui::EndDisabled();

                if (!obj.albedoTexturePath.empty()) {
                    ImGui::SameLine();
                    if (hasSpritesheetPackage() && ImGui::SmallButton("Import Sheet##UISpriteSheet")) {
                        pendingSpriteSheetPath = obj.albedoTexturePath;
                        std::snprintf(importSpriteSheetName, sizeof(importSpriteSheetName), "%s", obj.name.c_str());
                        importSpriteSheetTarget = (obj.type == ObjectType::Sprite25D)
                            ? SpriteSheetImportTarget::Sprite25D
                            : ((obj.ui.type == UIElementType::Sprite2D)
                                ? SpriteSheetImportTarget::Sprite2D
                                : SpriteSheetImportTarget::UIImage);
                        showImportSpriteSheetDialog = true;
                    }
                }

                if (Texture* previewTex = (!obj.albedoTexturePath.empty()
                        ? renderer.getTexture(obj.albedoTexturePath, MaterialProperties::TextureFilter::Point)
                        : nullptr)) {
                    if (previewTex->GetID()) {
                        ImGui::Spacing();
                        ImGui::TextDisabled("Sprite Preview");
                        std::array<ImVec2, 4> uvQuad = buildSpriteSheetUvs(obj);
                        ImVec2 uvMin(uvQuad[0].x, uvQuad[0].y);
                        ImVec2 uvMax(uvQuad[2].x, uvQuad[2].y);
                        float frameWidth = static_cast<float>(previewTex->GetWidth());
                        float frameHeight = static_cast<float>(previewTex->GetHeight());
                        if (obj.ui.spriteCustomFramesEnabled && !obj.ui.spriteCustomFrames.empty()) {
                            const glm::ivec4 frame = obj.ui.spriteCustomFrames[std::clamp(obj.ui.spriteSheetFrame, 0, static_cast<int>(obj.ui.spriteCustomFrames.size()) - 1)];
                            frameWidth = static_cast<float>(frame.z);
                            frameHeight = static_cast<float>(frame.w);
                        } else if (obj.ui.spriteSheetEnabled) {
                            frameWidth = std::max(1.0f, frameWidth / static_cast<float>(std::max(1, obj.ui.spriteSheetColumns)));
                            frameHeight = std::max(1.0f, frameHeight / static_cast<float>(std::max(1, obj.ui.spriteSheetRows)));
                        }
                        float previewWidth = std::min(ImGui::GetContentRegionAvail().x, 196.0f);
                        float aspect = frameWidth > 0.0f ? (frameHeight / frameWidth) : 1.0f;
                        ImVec2 previewSize(previewWidth, std::max(64.0f, previewWidth * aspect));
                        ImGui::Image((ImTextureID)(intptr_t)previewTex->GetID(), previewSize, uvMin, uvMax);
                    }
                }

                auto uiMaterialHeaderMenu = [&]() {
                    const bool hasMaterialAsset = !obj.materialPath.empty();
                    if (hasMaterialAsset) {
                        const int users = countMaterialAssetUsers(obj.materialPath);
                        ImGui::TextDisabled(users > 1 ? "Shared by %d objects" : "Material asset", users);
                        ImGui::Separator();
                    }
                    if (ImGui::MenuItem("Reload Material", nullptr, false, hasMaterialAsset)) {
                        loadMaterialFromFile(obj);
                        propagateMaterialAsset(obj.materialPath, materialFileDataFromObject(obj), obj.id);
                        changed = true;
                    }
                    if (ImGui::MenuItem("Save Material", nullptr, false, hasMaterialAsset)) {
                        saveMaterialToFile(obj);
                    }
                    if (ImGui::MenuItem("Reload Shader", nullptr, false,
                                        !obj.vertexShaderPath.empty() || !obj.fragmentShaderPath.empty())) {
                        renderer.forceReloadShader(obj.vertexShaderPath, obj.fragmentShaderPath);
                    }
                };

                const std::string uiMaterialName = obj.materialPath.empty()
                    ? std::string("Image Material")
                    : assetDisplayName(obj.materialPath, "Image Material");
                auto uiMaterialHeaderState = drawMaterialSectionHeader(
                    "UIImageMaterialSection",
                    uiMaterialName,
                    obj.shaderPackPath,
                    obj.vertexShaderPath,
                    obj.fragmentShaderPath,
                    obj.material,
                    obj.albedoTexturePath,
                    obj.overlayTexturePath,
                    obj.normalMapPath,
                    obj.useOverlay,
                    true,
                    1204,
                    uiMaterialHeaderMenu);

                bool uiMaterialChanged = uiMaterialHeaderState.second;
                if (uiMaterialHeaderState.first) {
                    InspectorBodyScope _materialBody(*this);
                    uiMaterialChanged |= renderMaterialEditorBody(
                        "UIImageMaterial",
                        obj.material,
                        obj.albedoTexturePath,
                        obj.overlayTexturePath,
                        obj.normalMapPath,
                        obj.metallicRoughnessMapPath,
                        obj.metallicMapPath,
                        obj.roughnessMapPath,
                        obj.opacityMapPath,
                        obj.emissiveMapPath,
                        obj.occlusionMapPath,
                        obj.heightMapPath,
                        obj.useOverlay,
                        obj.shaderPackPath,
                        obj.vertexShaderPath,
                        obj.fragmentShaderPath,
                        nullptr,
                        uiImageMaterialPreviewScale,
                        1205);
                }

                static int pendingUiImageMaterialSaveId = -1;
                if (uiMaterialChanged) {
                    changed = true;
                    if (!obj.materialPath.empty()) {
                        propagateMaterialAsset(obj.materialPath, materialFileDataFromObject(obj), obj.id);
                        pendingUiImageMaterialSaveId = obj.id;
                    }
                }
                if (pendingUiImageMaterialSaveId == obj.id && !ImGui::IsAnyItemActive()) {
                    pendingUiImageMaterialSaveId = -1;
                    if (!obj.materialPath.empty()) {
                        const std::string filePath = resolveMaterialAssetPath(obj.materialPath).string();
                        if (!writeMaterialFile(materialFileDataFromObject(obj), filePath)) {
                            addConsoleMessage("Failed to save material: " + filePath,
                                              ConsoleMessageType::Error);
                        }
                    }
                }

                if (InspectorReveal _fs = drawInspectorSubsectionFoldout(
                        "Sprite Sheet", nullptr, true, hasSpritesheetPackage())) {
                    if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_ENABLE_SPRITE_SHEET", "Enable Sprite Sheet"), &obj.ui.spriteSheetEnabled)) {
                        changed = true;
                    }
                    ImGui::BeginDisabled(!obj.ui.spriteSheetEnabled);
                    const bool usingCustomClips = obj.ui.spriteCustomFramesEnabled && !obj.ui.spriteCustomFrames.empty();
                    if (usingCustomClips) {
                        const int clipCount = static_cast<int>(obj.ui.spriteCustomFrames.size());
                        EnsureSpriteClipNames(obj.ui.spriteCustomFrameNames, obj.ui.spriteCustomFrames.size());
                        ImGui::TextDisabled("Using %d cropped sprite clips.", clipCount);
                        obj.ui.spriteSheetFrame = std::clamp(obj.ui.spriteSheetFrame, 0, clipCount - 1);
                        const char* previewName = obj.ui.spriteCustomFrameNames[obj.ui.spriteSheetFrame].c_str();
                        if (ImGui::BeginCombo("Clip", previewName)) {
                            for (int clipIndex = 0; clipIndex < clipCount; ++clipIndex) {
                                bool selected = (clipIndex == obj.ui.spriteSheetFrame);
                                if (ImGui::Selectable(obj.ui.spriteCustomFrameNames[clipIndex].c_str(), selected)) {
                                    obj.ui.spriteSheetFrame = clipIndex;
                                    changed = true;
                                }
                                if (selected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        int clipIndex = obj.ui.spriteSheetFrame;
                        if (ImGui::SliderInt("Clip Index", &clipIndex, 0, clipCount - 1)) {
                            obj.ui.spriteSheetFrame = std::clamp(clipIndex, 0, clipCount - 1);
                            changed = true;
                        }
                    } else {
                        if (ImGui::DragInt("Columns", &obj.ui.spriteSheetColumns, 1.0f, 1, 1024)) {
                            obj.ui.spriteSheetColumns = std::max(1, obj.ui.spriteSheetColumns);
                            changed = true;
                        }
                        if (ImGui::DragInt("Rows", &obj.ui.spriteSheetRows, 1.0f, 1, 1024)) {
                            obj.ui.spriteSheetRows = std::max(1, obj.ui.spriteSheetRows);
                            changed = true;
                        }
                        int frameCount = std::max(1, obj.ui.spriteSheetColumns * obj.ui.spriteSheetRows);
                        if (ImGui::SliderInt("Frame", &obj.ui.spriteSheetFrame, 0, frameCount - 1)) {
                            obj.ui.spriteSheetFrame = std::clamp(obj.ui.spriteSheetFrame, 0, frameCount - 1);
                            changed = true;
                        }
                        if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_FPS", "FPS"), &obj.ui.spriteSheetFps, 0.1f, 1.0f, 120.0f, "%.1f")) {
                            obj.ui.spriteSheetFps = std::clamp(obj.ui.spriteSheetFps, 1.0f, 120.0f);
                            changed = true;
                        }
                        if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_LOOP", "Loop"), &obj.ui.spriteSheetLoop)) {
                            changed = true;
                        }
                    }
                    ImGui::EndDisabled();
                }

                if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Scroll UV", nullptr, true)) {
                    if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_ENABLE_UV_SCROLL", "Enable UV Scroll"),
                                        &obj.material.uvScrollEnabled)) {
                        changed = true;
                    }
                    ImGui::BeginDisabled(!obj.material.uvScrollEnabled);
                    if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_SCROLL_SPEED", "Scroll Speed"),
                                         &obj.material.scrollSpeed, 0.01f, 0.0f, 0.0f, "%.3f")) {
                        obj.material.scrollSpeed = std::max(0.0f, obj.material.scrollSpeed);
                        changed = true;
                    }
                    if (ImGui::DragFloat2(Loc::Widget("COMPONENT_UI_SCROLL_DIRECTION", "Scroll Direction"),
                                          &obj.material.scrollDirection.x, 0.01f, 0.0f, 0.0f, "%.2f")) {
                        changed = true;
                    }
                    ImGui::EndDisabled();
                }

                if (InspectorReveal _fs = drawInspectorSubsectionFoldout("9-Slice")) {
                    if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_ENABLE_9_SLICE", "Enable 9-Slice"), &obj.ui.nineSliceEnabled)) {
                        changed = true;
                    }
                    ImGui::BeginDisabled(!obj.ui.nineSliceEnabled);
                    float border[4] = {
                        obj.ui.nineSliceBorder.x,
                        obj.ui.nineSliceBorder.y,
                        obj.ui.nineSliceBorder.z,
                        obj.ui.nineSliceBorder.w
                    };
                    if (ImGui::DragFloat4("Border L/R/T/B", border, 1.0f, 0.0f, 2048.0f, "%.0f")) {
                        obj.ui.nineSliceBorder = glm::vec4(
                            std::max(0.0f, border[0]),
                            std::max(0.0f, border[1]),
                            std::max(0.0f, border[2]),
                            std::max(0.0f, border[3]));
                        changed = true;
                    }
                    if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_TILE_EDGES", "Tile Edges"), &obj.ui.nineSliceTileEdges)) {
                        changed = true;
                    }
                    if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_TILE_CENTER", "Tile Center"), &obj.ui.nineSliceTileCenter)) {
                        changed = true;
                    }
                    ImGui::EndDisabled();
                }

                if (InspectorReveal _fs = drawInspectorSubsectionFoldout("2D Lighting", nullptr, true)) {
                    if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_RECEIVE_LIGHTING", "Receive Lighting"), &obj.ui.receiveLighting2D)) {
                        changed = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Checkbox(Loc::Widget("COMPONENT_UI_FORCE_UNLIT", "Force Unlit"), &obj.ui.unlitLighting2D)) {
                        changed = true;
                    }
                    if (ImGui::SliderFloat(Loc::Widget("COMPONENT_UI_EMISSIVE", "Emissive"), &obj.ui.emissiveLighting2D, 0.0f, 8.0f, "%.2f")) {
                        changed = true;
                    }
                }
            }

            if (obj.ui.type == UIElementType::Slider) {
                const char* sliderStyles[] = { "ImGui", "Fill", "Circle", "Vertical", "Ring", "Stepped" };
                int sliderStyle = static_cast<int>(obj.ui.sliderStyle);
                if (ImGui::Combo(Loc::Widget("COMPONENT_UI_STYLE", "Style"), &sliderStyle, sliderStyles, IM_ARRAYSIZE(sliderStyles))) {
                    obj.ui.sliderStyle = static_cast<UISliderStyle>(sliderStyle);
                    changed = true;
                }
                if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_MIN", "Min"), &obj.ui.sliderMin, 0.1f)) {
                    changed = true;
                }
                if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_MAX", "Max"), &obj.ui.sliderMax, 0.1f)) {
                    changed = true;
                }
                if (obj.ui.sliderMax < obj.ui.sliderMin) {
                    std::swap(obj.ui.sliderMin, obj.ui.sliderMax);
                }
                if (ImGui::SliderFloat(Loc::Widget("COMPONENT_UI_VALUE", "Value"), &obj.ui.sliderValue, obj.ui.sliderMin, obj.ui.sliderMax)) {
                    changed = true;
                }
            }

            ImVec4 uiColor(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b, obj.ui.color.a);
            if (selectedValuesMixed([](const SceneObject& selectedObj) { return selectedObj.ui.color; })) {
                if (ImGui::Button("--##MixedTint", ImVec2(-FLT_MIN, 0.0f))) {
                    ImGui::OpenPopup("##MixedValuePopup_Tint");
                }
                if (ImGui::BeginPopup("##MixedValuePopup_Tint")) {
                    ImGui::TextUnformatted("Which value would you like to change it to?");
                    ImGui::Separator();
                    for (SceneObject* selectedObj : selectedObjects) {
                        if (!selectedObj) continue;
                        const glm::vec4 color = selectedObj->ui.color;
                        char colorLabel[256] = {};
                        std::snprintf(colorLabel, sizeof(colorLabel), "%s (R:%d G:%d B:%d A:%d)##%d",
                                      selectedObj->name.c_str(),
                                      static_cast<int>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
                                      static_cast<int>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
                                      static_cast<int>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
                                      static_cast<int>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f),
                                      selectedObj->id);
                        if (ImGui::Selectable(colorLabel)) {
                            obj.ui.color = color;
                            changed = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::Selectable("Clear Value")) {
                        obj.ui.color = glm::vec4(1.0f);
                        changed = true;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            } else if (ImGui::ColorEdit4(Loc::Widget("COMPONENT_UI_TINT", "Tint"), &uiColor.x)) {
                obj.ui.color = glm::vec4(uiColor.x, uiColor.y, uiColor.z, uiColor.w);
                changed = true;
            }

            if (obj.ui.type == UIElementType::Button) {
                const char* buttonStyles[] = { "ImGui", "Outline" };
                int buttonStyle = static_cast<int>(obj.ui.buttonStyle);
                if (ImGui::Combo(Loc::Widget("COMPONENT_UI_STYLE_2", "Style"), &buttonStyle, buttonStyles, IM_ARRAYSIZE(buttonStyles))) {
                    obj.ui.buttonStyle = static_cast<UIButtonStyle>(buttonStyle);
                    changed = true;
                }
                ImGui::TextDisabled("Last Pressed: %s", obj.ui.buttonPressed ? "yes" : "no");
            }

            if (obj.ui.type == UIElementType::Slider || obj.ui.type == UIElementType::Button) {
                if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Color Overrides")) {
                    ImGui::TextDisabled("Alpha = 0 means use default derived color.");
                    ImVec4 fillCol(obj.ui.fillColor.r, obj.ui.fillColor.g, obj.ui.fillColor.b, obj.ui.fillColor.a);
                    if (ImGui::ColorEdit4(Loc::Widget("COMPONENT_UI_FILL_COLOR", "Fill Color"), &fillCol.x)) {
                        obj.ui.fillColor = glm::vec4(fillCol.x, fillCol.y, fillCol.z, fillCol.w);
                        changed = true;
                    }
                    ImVec4 bgCol(obj.ui.backgroundColor.r, obj.ui.backgroundColor.g, obj.ui.backgroundColor.b, obj.ui.backgroundColor.a);
                    if (ImGui::ColorEdit4(Loc::Widget("COMPONENT_UI_BACKGROUND_COLOR", "Background Color"), &bgCol.x)) {
                        obj.ui.backgroundColor = glm::vec4(bgCol.x, bgCol.y, bgCol.z, bgCol.w);
                        changed = true;
                    }
                    ImVec4 borderCol(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a);
                    if (ImGui::ColorEdit4(Loc::Widget("COMPONENT_UI_BORDER_COLOR", "Border Color"), &borderCol.x)) {
                        obj.ui.borderColor = glm::vec4(borderCol.x, borderCol.y, borderCol.z, borderCol.w);
                        changed = true;
                    }
                    ImVec4 textCol(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a);
                    if (ImGui::ColorEdit4(Loc::Widget("COMPONENT_UI_LABEL_COLOR", "Label Color"), &textCol.x)) {
                        obj.ui.textColor = glm::vec4(textCol.x, textCol.y, textCol.z, textCol.w);
                        changed = true;
                    }
                    if (obj.ui.type == UIElementType::Slider) {
                        if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_LABEL_FONT_SIZE", "Label Font Size"), &obj.ui.fontSize, 0.5f, 0.0f, 128.0f, "%.1f")) {
                            obj.ui.fontSize = std::max(0.0f, obj.ui.fontSize);
                            changed = true;
                        }
                        ImGui::TextDisabled("Font Size = 0 inherits from Text Scale.");
                    }
                }
            }

            if (obj.ui.type == UIElementType::Text) {
                ImVec4 textCol(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a);
                if (ImGui::ColorEdit4(Loc::Widget("COMPONENT_UI_TEXT_COLOR_OVERRIDE", "Text Color Override"), &textCol.x)) {
                    obj.ui.textColor = glm::vec4(textCol.x, textCol.y, textCol.z, textCol.w);
                    changed = true;
                }
                ImGui::TextDisabled("Alpha = 0 uses the Tint color.");
            }

            if (ImGui::DragInt("Sort Order", &obj.ui.sortingOrder, 1.0f, -2048, 2048)) {
                changed = true;
            }

            if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_BACKDROP_BLUR", "Backdrop Blur"),
                                 &obj.ui.backdropBlur, 0.01f, 0.0f, 1.0f, "%.2f")) {
                obj.ui.backdropBlur = std::clamp(obj.ui.backdropBlur, 0.0f, 1.0f);
                changed = true;
            }
            if (obj.ui.backdropBlur > 0.0f) {
                if (ImGui::DragFloat(Loc::Widget("COMPONENT_UI_BACKDROP_ROUNDING", "Backdrop Rounding"),
                                     &obj.ui.backdropRounding, 0.5f, 0.0f, 256.0f, "%.1f")) {
                    obj.ui.backdropRounding = std::max(0.0f, obj.ui.backdropRounding);
                    changed = true;
                }
                ImGui::TextDisabled("Frosts whatever the game drew behind this element.\n"
                                    "Sorting Order decides what counts as \"behind\". OpenGL only.");
            }

            ImGui::PopID();
        }
        if (removeUi) {
            obj.hasUI = false;
            obj.ui.type = UIElementType::None;
            UpdateLegacyTypeFromComponents(obj);
            changed = true;
        }
        if (changed) {
            uiSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "collider" && obj.hasCollider && sharedCollider) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.5f, 0.35f, 1.0f));
        bool removeCollider = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_COLLIDER", "Collider"), "Collider", "collider", &obj.collider.enabled, true, [&]() {
            drawStandardComponentMenu(
                "collider",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Collider,
                [&]() { obj.collider = ColliderComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::Collider; inspectorClipboard.collider = obj.collider; },
                [&]() { obj.hasCollider = true; obj.collider = inspectorClipboard.collider; changed = true; },
                [&]() { obj.collider = inspectorClipboard.collider; changed = true; },
                removeCollider);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Collider");

            if (beginCompFields("##Fields_Collider")) {
                const char* colliderTypes[] = { "Box", "Mesh", "Convex Mesh", "Capsule", "Sphere" };
                int colliderType = static_cast<int>(obj.collider.type);
                fieldRow(Loc::T("COMPONENT_COLLIDER_TYPE", "Type"));
                if (ImGui::Combo("##Type", &colliderType, colliderTypes, IM_ARRAYSIZE(colliderTypes))) {
                    obj.collider.type = static_cast<ColliderType>(colliderType);
                    if (obj.collider.type == ColliderType::Mesh) {
                        obj.collider.convex = false;
                    } else if (obj.collider.type == ColliderType::ConvexMesh) {
                        obj.collider.convex = true;
                    }
                    changed = true;
                }

                if (obj.collider.type == ColliderType::Box) {
                    fieldRow(Loc::T("COMPONENT_COLLIDER_BOX_SIZE", "Box Size"));
                    if (ImGui::DragFloat3("##BoxSize", &obj.collider.boxSize.x, 0.01f, 0.01f, 1000.0f, "%.3f")) {
                        obj.collider.boxSize.x = std::max(0.01f, obj.collider.boxSize.x);
                        obj.collider.boxSize.y = std::max(0.01f, obj.collider.boxSize.y);
                        obj.collider.boxSize.z = std::max(0.01f, obj.collider.boxSize.z);
                        changed = true;
                    }
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1);
                    if (ImGui::SmallButton("Match Object Scale")) {
                        obj.collider.boxSize = glm::max(obj.scale, glm::vec3(0.01f));
                        changed = true;
                    }
                } else if (obj.collider.type == ColliderType::Capsule) {
                    float radius = std::max(0.05f, std::max(obj.collider.boxSize.x, obj.collider.boxSize.z) * 0.5f);
                    float height = std::max(0.1f, obj.collider.boxSize.y);
                    fieldRow(Loc::T("COMPONENT_COLLIDER_RADIUS", "Radius"));
                    if (ImGui::DragFloat("##Radius", &radius, 0.01f, 0.05f, 5.0f, "%.3f")) {
                        obj.collider.boxSize.x = obj.collider.boxSize.z = radius * 2.0f;
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_COLLIDER_HEIGHT", "Height"));
                    if (ImGui::DragFloat("##Height", &height, 0.01f, 0.1f, 10.0f, "%.3f")) {
                        obj.collider.boxSize.y = height;
                        changed = true;
                    }
                    noteRow("Capsule aligned to Y axis.");
                } else if (obj.collider.type == ColliderType::Sphere) {
                    float radius = std::max(0.01f, std::max({obj.collider.boxSize.x, obj.collider.boxSize.y, obj.collider.boxSize.z}) * 0.5f);
                    fieldRow(Loc::T("COMPONENT_COLLIDER_RADIUS", "Radius"));
                    if (ImGui::DragFloat("##SphereRadius", &radius, 0.01f, 0.01f, 100.0f, "%.3f")) {
                        obj.collider.boxSize = glm::vec3(std::max(0.01f, radius) * 2.0f);
                        changed = true;
                    }
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1);
                    if (ImGui::SmallButton("Match Object Scale")) {
                        obj.collider.boxSize = glm::vec3(std::max(0.01f, std::max({obj.scale.x, obj.scale.y, obj.scale.z})));
                        changed = true;
                    }
                }

                fieldRow(Loc::T("COMPONENT_COLLIDER_OFFSET", "Offset"));
                if (ImGui::DragFloat3("##Offset", &obj.collider.offset.x, 0.01f, -1000.0f, 1000.0f, "%.3f")) {
                    changed = true;
                }

                if (obj.collider.type == ColliderType::Box ||
                    obj.collider.type == ColliderType::Sphere ||
                    obj.collider.type == ColliderType::Mesh ||
                    obj.collider.type == ColliderType::ConvexMesh) {
                    fieldRow(Loc::T("COMPONENT_COLLIDER_IS_TRIGGER", "Is Trigger"));
                    if (ImGui::Checkbox("##IsTrigger", &obj.collider.isTrigger)) {
                        changed = true;
                    }
                    if (obj.collider.isTrigger && obj.collider.type == ColliderType::Mesh) {
                        noteRow("PhysX uses a convex trigger hull; Jolt keeps the authored mesh shape.");
                    }
                }

                endCompFields();
            }

            ImGui::SeparatorText("Surface");
            if (beginCompFields("##Fields_ColliderSurface")) {
                fieldRow(Loc::T("COMPONENT_COLLIDER_STATIC_FRICTION", "Static Friction"));
                if (ImGui::DragFloat("##StaticFriction", &obj.collider.staticFriction, 0.01f, 0.0f, 4.0f, "%.2f")) {
                    obj.collider.staticFriction = std::clamp(obj.collider.staticFriction, 0.0f, 4.0f);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_COLLIDER_DYN_FRICTION", "Dyn Friction"));
                if (ImGui::DragFloat("##DynFriction", &obj.collider.dynamicFriction, 0.01f, 0.0f, 4.0f, "%.2f")) {
                    obj.collider.dynamicFriction = std::clamp(obj.collider.dynamicFriction, 0.0f, 4.0f);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_COLLIDER_RESTITUTION", "Restitution"));
                if (ImGui::DragFloat("##Restitution", &obj.collider.restitution, 0.01f, 0.0f, 1.0f, "%.2f")) {
                    obj.collider.restitution = std::clamp(obj.collider.restitution, 0.0f, 1.0f);
                    changed = true;
                }
                endCompFields();
            }

            ImGui::PopID();
        }
        if (removeCollider) {
            obj.hasCollider = false;
            changed = true;
        }
        if (changed) {
            colliderSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "player_controller" && obj.hasPlayerController && sharedPlayerController) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.45f, 0.7f, 1.0f));
        bool removePlayerController = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_PLAYER_CONTROLLER", "Player Controller"), "PlayerController", "player_controller", &obj.playerController.enabled, true, [&]() {
            drawStandardComponentMenu(
                "player_controller",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::PlayerController,
                [&]() { obj.playerController = PlayerControllerComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::PlayerController; inspectorClipboard.playerController = obj.playerController; },
                [&]() { obj.hasPlayerController = true; obj.playerController = inspectorClipboard.playerController; changed = true; },
                [&]() { obj.playerController = inspectorClipboard.playerController; changed = true; },
                removePlayerController);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("PlayerController");
            if (beginCompFields("##Fields_PlayerController")) {
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_MOVE_SPEED", "Move Speed"));
                if (ImGui::DragFloat("##MoveSpeed", &obj.playerController.moveSpeed, 0.1f, 0.1f, 100.0f, "%.2f")) {
                    obj.playerController.moveSpeed = std::max(0.1f, obj.playerController.moveSpeed);
                    obj.playerController.runSpeed = std::max(obj.playerController.moveSpeed, obj.playerController.runSpeed);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_RUN_SPEED", "Run Speed"));
                if (ImGui::DragFloat("##RunSpeed", &obj.playerController.runSpeed, 0.1f, 0.1f, 140.0f, "%.2f")) {
                    obj.playerController.runSpeed = std::max(obj.playerController.moveSpeed, obj.playerController.runSpeed);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_LOOK_SENS", "Look Sens."));
                if (ImGui::DragFloat("##LookSensitivity", &obj.playerController.lookSensitivity, 0.01f, 0.01f, 2.0f, "%.2f")) {
                    obj.playerController.lookSensitivity = std::clamp(obj.playerController.lookSensitivity, 0.01f, 2.0f);
                    changed = true;
                }
                fieldRow("Look Smoothing");
                if (ImGui::DragFloat("##LookSmoothing", &obj.playerControlFeel.lookSmoothing, 0.01f, 0.0f, 1.0f, "%.2f")) {
                    obj.playerControlFeel.lookSmoothing = std::clamp(obj.playerControlFeel.lookSmoothing, 0.0f, 1.0f);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("0 is raw one-to-one mouse look. Higher spreads each\n"
                                      "movement over a short window without changing how\n"
                                      "far it turns you - sensitivity is unaffected.");
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_GROUND_ACCEL", "Ground Accel"));
                if (ImGui::DragFloat("##GroundAccel", &obj.playerController.groundAcceleration, 0.1f, 0.0f, 200.0f, "%.2f")) {
                    obj.playerController.groundAcceleration = std::clamp(obj.playerController.groundAcceleration, 0.0f, 200.0f);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_AIR_ACCEL", "Air Accel"));
                if (ImGui::DragFloat("##AirAccel", &obj.playerController.airAcceleration, 0.1f, 0.0f, 200.0f, "%.2f")) {
                    obj.playerController.airAcceleration = std::clamp(obj.playerController.airAcceleration, 0.0f, 200.0f);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_BRAKING", "Braking"));
                if (ImGui::DragFloat("##Braking", &obj.playerController.braking, 0.1f, 0.0f, 200.0f, "%.2f")) {
                    obj.playerController.braking = std::clamp(obj.playerController.braking, 0.0f, 200.0f);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_MIN_SURF_CTRL", "Min Surf Ctrl"));
                if (ImGui::DragFloat("##MinSurfaceControl", &obj.playerController.minSurfaceControl, 0.01f, 0.0f, 1.0f, "%.2f")) {
                    obj.playerController.minSurfaceControl = std::clamp(obj.playerController.minSurfaceControl, 0.0f, 1.0f);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_SLIDE_GRAVITY", "Slide Gravity"));
                if (ImGui::DragFloat("##SlideGravity", &obj.playerController.slideGravity, 0.1f, 0.0f, 120.0f, "%.2f")) {
                    obj.playerController.slideGravity = std::clamp(obj.playerController.slideGravity, 0.0f, 120.0f);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_PLATFORM_CARRY", "Platform Carry"));
                if (ImGui::DragFloat("##PlatformCarry", &obj.playerController.platformCarry, 0.01f, 0.0f, 3.0f, "%.2f")) {
                    obj.playerController.platformCarry = std::clamp(obj.playerController.platformCarry, 0.0f, 3.0f);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_HEIGHT", "Height"));
                if (ImGui::DragFloat("##Height", &obj.playerController.height, 0.01f, 0.5f, 3.0f, "%.2f")) {
                    obj.playerController.height = std::clamp(obj.playerController.height, 0.5f, 3.0f);
                    obj.scale.y = obj.playerController.height;
                    obj.collider.boxSize.y = obj.playerController.height;
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_RADIUS", "Radius"));
                if (ImGui::DragFloat("##Radius", &obj.playerController.radius, 0.01f, 0.2f, 1.2f, "%.2f")) {
                    obj.playerController.radius = std::clamp(obj.playerController.radius, 0.2f, 1.2f);
                    obj.scale.x = obj.scale.z = obj.playerController.radius * 2.0f;
                    obj.collider.boxSize.x = obj.collider.boxSize.z = obj.playerController.radius * 2.0f;
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_PLAYER_CONTROLLER_JUMP_STRENGTH", "Jump Strength"));
                if (ImGui::DragFloat("##JumpStrength", &obj.playerController.jumpStrength, 0.1f, 0.1f, 30.0f, "%.1f")) {
                    obj.playerController.jumpStrength = std::max(0.1f, obj.playerController.jumpStrength);
                    changed = true;
                }
                if (boolRow("Charged Jump", &obj.playerControlFeel.chargedJump)) { changed = true; }
                if (obj.playerControlFeel.chargedJump) {
                    noteRow("Hold to wind up, release to launch.");
                    fieldRow("Charge Time");
                    if (ImGui::DragFloat("##JumpChargeTime", &obj.playerControlFeel.jumpChargeTime, 0.01f, 0.01f, 2.0f, "%.2f")) {
                        obj.playerControlFeel.jumpChargeTime = std::clamp(obj.playerControlFeel.jumpChargeTime, 0.01f, 2.0f);
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Seconds to reach Jump Strength. Holding past this\n"
                                          "launches automatically, so holding space still\n"
                                          "gives repeated jumps like it always did.");
                    }
                    fieldRow("Tap Height");
                    if (ImGui::DragFloat("##JumpMinScale", &obj.playerControlFeel.jumpChargeMinScale, 0.01f, 0.0f, 1.0f, "%.2f")) {
                        obj.playerControlFeel.jumpChargeMinScale = std::clamp(obj.playerControlFeel.jumpChargeMinScale, 0.0f, 1.0f);
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Fraction of Jump Strength a bare tap gives.");
                    }
                    if (!obj.playerViewMotion.enabled) {
                        noteRow("Enable View Motion below for the wind-up crouch.");
                    }
                }
                endCompFields();
            }

            if (InspectorReveal _ma = drawInspectorSubsectionFoldout("Movement Audio", nullptr, true)) {
                PlayerMovementAudioSettings& ma = obj.playerMovementAudio;
                if (beginCompFields("##Fields_PlayerMoveAudio")) {
                    if (boolRow("Enabled", &ma.enabled)) { changed = true; }
                    endCompFields();
                }

                if (ma.enabled) {
                    ImGui::PushID("MovementAudioClips");
                    ImGui::TextDisabled("Footsteps");
                    if (ma.footstepClips.empty()) {
                        if (beginCompFields("##Fields_MoveAudioEmpty")) {
                            noteRow("Add clips to hear footsteps. One is picked at random per step.");
                            endCompFields();
                        }
                    }

                    int removeFootstepIndex = -1;
                    for (size_t clipIndex = 0; clipIndex < ma.footstepClips.size(); ++clipIndex) {
                        ImGui::PushID(static_cast<int>(clipIndex));
                        char slotLabel[48] = {};
                        std::snprintf(slotLabel, sizeof(slotLabel), "Step %d", static_cast<int>(clipIndex) + 1);
                        if (drawAssetReferenceSlot("##StepClip", slotLabel, ma.footstepClips[clipIndex],
                                                   FileCategory::Audio, "Sound Clip")) {
                            changed = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            removeFootstepIndex = static_cast<int>(clipIndex);
                        }
                        ImGui::PopID();
                    }
                    if (removeFootstepIndex >= 0) {
                        ma.footstepClips.erase(ma.footstepClips.begin() + removeFootstepIndex);
                        changed = true;
                    }

                    if (ImGui::SmallButton("Add Footstep Clip")) {
                        ma.footstepClips.emplace_back();
                        changed = true;
                    }
                    if (ma.footstepClips.size() > 1) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%d clips)", static_cast<int>(ma.footstepClips.size()));
                    }
                    ImGui::PopID();

                    ImGui::Spacing();
                    if (drawAssetReferenceSlot("##JumpClipSlot", "Jump", ma.jumpClip,
                                               FileCategory::Audio, "Sound Clip")) {
                        changed = true;
                    }
                    if (drawAssetReferenceSlot("##LandClipSlot", "Land", ma.landClip,
                                               FileCategory::Audio, "Sound Clip")) {
                        changed = true;
                    }

                    if (beginCompFields("##Fields_MoveAudioMix")) {
                        fieldRow("Volume");
                        if (ImGui::DragFloat("##MoveAudioVolume", &ma.volume, 0.01f, 0.0f, 2.0f, "%.2f")) {
                            ma.volume = std::clamp(ma.volume, 0.0f, 2.0f);
                            changed = true;
                        }
                        fieldRow("Run Volume");
                        if (ImGui::DragFloat("##MoveAudioRunVolume", &ma.runVolumeScale, 0.01f, 0.0f, 3.0f, "%.2f")) {
                            ma.runVolumeScale = std::clamp(ma.runVolumeScale, 0.0f, 3.0f);
                            changed = true;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Footstep volume scale at full run.");
                        }
                        fieldRow("Land Volume");
                        if (ImGui::DragFloat("##MoveAudioLandVolume", &ma.landVolumeScale, 0.01f, 0.0f, 3.0f, "%.2f")) {
                            ma.landVolumeScale = std::clamp(ma.landVolumeScale, 0.0f, 3.0f);
                            changed = true;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Scaled again by how hard the landing was, so a\n"
                                              "hop is quieter than a drop off a roof.");
                        }
                        fieldRow("Pitch Variance");
                        if (ImGui::DragFloat("##MoveAudioPitch", &ma.pitchVariance, 0.01f, 0.0f, 0.9f, "%.2f")) {
                            ma.pitchVariance = std::clamp(ma.pitchVariance, 0.0f, 0.9f);
                            changed = true;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Random pitch shift per sound, so the same clip\n"
                                              "does not read as a loop. 0 plays them untouched.");
                        }
                        endCompFields();
                    }

                    if (beginCompFields("##Fields_MoveAudioNote")) {
                        noteRow("Step rate follows View Motion > Walk / Run Bob > Frequency.");
                        endCompFields();
                    }
                }
            }

            if (InspectorReveal _vm = drawInspectorSubsectionFoldout("View Motion", nullptr, true)) {
                PlayerViewMotionSettings& vm = obj.playerViewMotion;
                if (beginCompFields("##Fields_PlayerViewMotion")) {
                    if (boolRow("Enabled", &vm.enabled)) { changed = true; }
                    if (!vm.enabled) {
                        noteRow("Head bob, look sway and idle breathing are off.");
                    }
                    endCompFields();
                }

                if (vm.enabled) {
                    auto motionSlider = [&](const char* label, const char* id, float* value,
                                            float speed, float minValue, float maxValue,
                                            const char* format, const char* tooltip) {
                        fieldRow(label);
                        if (ImGui::DragFloat(id, value, speed, minValue, maxValue, format)) {
                            *value = std::clamp(*value, minValue, maxValue);
                            changed = true;
                        }
                        if (tooltip && ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", tooltip);
                        }
                    };

                    if (InspectorReveal _b = drawInspectorSubsectionFoldout("Walk / Run Bob", nullptr, true)) {
                        if (beginCompFields("##Fields_VmBob")) {
                            motionSlider("Frequency", "##VmBobFrequency", &vm.bobFrequency,
                                         0.01f, 0.0f, 4.0f, "%.2f",
                                         "Stride cycles per second at Move Speed. Scales with actual speed.");
                            motionSlider("Vertical", "##VmBobVertical", &vm.bobVertical,
                                         0.001f, 0.0f, 0.5f, "%.3f",
                                         "Camera rise and fall, in units. Runs at twice the stride rate.");
                            motionSlider("Horizontal", "##VmBobHorizontal", &vm.bobHorizontal,
                                         0.001f, 0.0f, 0.5f, "%.3f",
                                         "Side-to-side travel, in units. One cycle per stride.");
                            motionSlider("Roll", "##VmBobRoll", &vm.bobRoll,
                                         0.01f, 0.0f, 10.0f, "%.2f",
                                         "Degrees of roll per stride.");
                            motionSlider("Run Multiplier", "##VmRunMultiplier", &vm.runMultiplier,
                                         0.01f, 0.0f, 4.0f, "%.2f",
                                         "Amplitude scale once you reach Run Speed.");
                            motionSlider("Run Lean", "##VmRunLean", &vm.runLean,
                                         0.01f, -8.0f, 8.0f, "%.2f",
                                         "Degrees the head pitches forward at full run.\n"
                                         "Gives the walk-to-run change something to travel through.");
                            motionSlider("Gait Blend", "##VmGaitBlend", &vm.gaitBlend,
                                         0.1f, 0.1f, 40.0f, "%.1f",
                                         "How fast the gait eases between idle, walk and run.\n"
                                         "Lower is a longer changeover. Independent of Ground\n"
                                         "Accel, which is normally tuned near-instant.");
                            endCompFields();
                        }
                    }

                    if (InspectorReveal _l = drawInspectorSubsectionFoldout("Look Sway", nullptr, true)) {
                        if (beginCompFields("##Fields_VmLook")) {
                            motionSlider("Amount", "##VmLookSway", &vm.lookSway,
                                         0.01f, 0.0f, 8.0f, "%.2f",
                                         "How hard a mouse flick throws the view. 0 disables look sway.");
                            motionSlider("Stiffness", "##VmLookStiffness", &vm.lookSwayStiffness,
                                         0.1f, 0.1f, 200.0f, "%.1f",
                                         "How fast the view springs back to centre.");
                            motionSlider("Damping", "##VmLookDamping", &vm.lookSwayDamping,
                                         0.1f, 0.0f, 60.0f, "%.1f",
                                         "Lower damping overshoots and wobbles; higher settles dead.");
                            motionSlider("Max Angle", "##VmLookMax", &vm.lookSwayMax,
                                         0.1f, 0.0f, 30.0f, "%.1f",
                                         "Hard ceiling on the sway, in degrees.");
                            endCompFields();
                        }
                    }

                    if (InspectorReveal _r = drawInspectorSubsectionFoldout("Lean", nullptr, true)) {
                        if (beginCompFields("##Fields_VmLean")) {
                            motionSlider("Strafe Roll", "##VmStrafeRoll", &vm.strafeRoll,
                                         0.01f, -10.0f, 10.0f, "%.2f",
                                         "Degrees of lean at full sideways speed. Negate to lean the other way.");
                            motionSlider("Turn Roll", "##VmTurnRoll", &vm.turnRoll,
                                         0.01f, -10.0f, 10.0f, "%.2f",
                                         "Degrees of lean into a fast turn (normalised to 180 deg/s).");
                            motionSlider("Smoothing", "##VmRollSmoothing", &vm.rollSmoothing,
                                         0.1f, 0.1f, 60.0f, "%.1f",
                                         "How quickly the lean follows. Lower is floatier.");
                            endCompFields();
                        }
                    }

                    if (InspectorReveal _i = drawInspectorSubsectionFoldout("Idle Breathing", nullptr, true)) {
                        if (beginCompFields("##Fields_VmIdle")) {
                            noteRow("Blends in as you stop moving.");
                            motionSlider("Amount", "##VmIdleAmount", &vm.idleAmount,
                                         0.01f, 0.0f, 6.0f, "%.2f",
                                         "Degrees of pitch drift while standing still. 0 disables it.");
                            motionSlider("Frequency", "##VmIdleFrequency", &vm.idleFrequency,
                                         0.01f, 0.01f, 3.0f, "%.2f",
                                         "Breaths per second. Two detuned sines, so it never repeats on the beat.");
                            motionSlider("Blend", "##VmIdleBlend", &vm.idleBlend,
                                         0.1f, 0.1f, 40.0f, "%.1f",
                                         "How fast breathing takes over once you stop.\n"
                                         "Below Gait Blend it eases in behind the footsteps\n"
                                         "dying away rather than rising in lockstep with them.");
                            endCompFields();
                        }
                    }

                    if (InspectorReveal _d = drawInspectorSubsectionFoldout("Landing & Jump Dip", nullptr, true)) {
                        if (beginCompFields("##Fields_VmLanding")) {
                            motionSlider("Jump Crouch", "##VmJumpCrouchDip", &vm.jumpCrouchDip,
                                         0.001f, 0.0f, 0.6f, "%.3f",
                                         "How far the camera sinks while a charged jump winds up.\n"
                                         "Needs Charged Jump enabled - an instant jump leaves no\n"
                                         "time to show the anticipation.");
                            motionSlider("Dip", "##VmLandingDip", &vm.landingDip,
                                         0.001f, 0.0f, 0.6f, "%.3f",
                                         "How far the camera drops on a hard landing, in units.");
                            motionSlider("Stiffness", "##VmLandingStiffness", &vm.landingStiffness,
                                         0.5f, 1.0f, 400.0f, "%.1f",
                                         "How fast it recovers. Dip depth stays as authored when you change this.");
                            motionSlider("Damping", "##VmLandingDamping", &vm.landingDamping,
                                         0.1f, 0.0f, 60.0f, "%.1f",
                                         "Lower damping gives a springier bounce back.");
                            endCompFields();
                        }
                    }

                    if (InspectorReveal _a = drawInspectorSubsectionFoldout("Attached Sway", nullptr, true)) {
                        if (beginCompFields("##Fields_VmAttached")) {
                            noteRow("Lags direct children (flashlight, held props) behind the aim.");
                            if (boolRow("Enabled", &vm.attachedSwayEnabled)) { changed = true; }
                            if (vm.attachedSwayEnabled) {
                                motionSlider("Amount", "##VmAttachedSway", &vm.attachedSway,
                                             0.01f, 0.0f, 8.0f, "%.2f",
                                             "How far a child swings behind a mouse flick.");
                                motionSlider("Stiffness", "##VmAttachedStiffness", &vm.attachedStiffness,
                                             0.1f, 0.1f, 200.0f, "%.1f",
                                             "How fast it catches back up to the aim.");
                                motionSlider("Damping", "##VmAttachedDamping", &vm.attachedDamping,
                                             0.1f, 0.0f, 60.0f, "%.1f",
                                             "Lower damping lets it swing past and back.");
                                motionSlider("Max Angle", "##VmAttachedMax", &vm.attachedMax,
                                             0.1f, 0.0f, 30.0f, "%.1f",
                                             "Hard ceiling on the lag, in degrees.");
                            }
                            endCompFields();
                        }
                    }

                    if (InspectorReveal _h = drawInspectorSubsectionFoldout("Held Item", nullptr, true)) {
                        if (beginCompFields("##Fields_VmHeld")) {
                            noteRow("A direct child with a light inside it (a flashlight model and its "
                                    "spot light) moves as though a hand is holding it.");
                            if (boolRow("Enabled", &vm.heldItemEnabled)) { changed = true; }
                            if (vm.heldItemEnabled) {
                                motionSlider("Walk / Run Bob", "##VmHeldBob", &vm.heldItemBob,
                                             0.01f, 0.0f, 4.0f, "%.2f",
                                             "Figure-eight swing that trails each stride, on top of\n"
                                             "the camera bob it already rides. 0 holds it steady.");
                                motionSlider("Idle", "##VmHeldIdle", &vm.heldItemIdle,
                                             0.01f, 0.0f, 4.0f, "%.2f",
                                             "Slow breathing drift while standing still.");
                                motionSlider("Jump", "##VmHeldJump", &vm.heldItemJump,
                                             0.01f, 0.0f, 4.0f, "%.2f",
                                             "Lag through a jump: dips on take-off, sinks while\n"
                                             "rising, floats while falling, drops on landing.");
                                motionSlider("Run Pose", "##VmHeldRunPose", &vm.heldItemRunPose,
                                             0.01f, 0.0f, 4.0f, "%.2f",
                                             "How far it tucks down and in at Run Speed. Mirrored\n"
                                             "automatically for an item held on the left.");
                            }
                            endCompFields();
                        }
                    }
                }
            }
            ImGui::PopID();
        }
        if (removePlayerController) {
            obj.hasPlayerController = false;
            changed = true;
        }
        if (changed) {
            if (obj.hasPlayerController) {
                obj.hasCollider = true;
                obj.collider.type = ColliderType::Capsule;
                obj.collider.convex = true;
                obj.hasRigidbody = true;
                obj.rigidbody.enabled = true;
                obj.rigidbody.useGravity = true;
                obj.rigidbody.lockRotationX = true;
                obj.rigidbody.lockRotationY = false;
                obj.rigidbody.lockRotationZ = true;
            }
            playerControllerSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "rigidbody3d" && obj.hasRigidbody && sharedRigidbody) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.45f, 0.45f, 0.25f, 1.0f));
        bool removeRigidbody = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_RIGIDBODY3_D", "Rigidbody3D"), "Rigidbody3D", "rigidbody3d", &obj.rigidbody.enabled, true, [&]() {
            drawStandardComponentMenu(
                "rigidbody3d",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Rigidbody3D,
                [&]() { obj.rigidbody = RigidbodyComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::Rigidbody3D; inspectorClipboard.rigidbody = obj.rigidbody; },
                [&]() { obj.hasRigidbody = true; obj.rigidbody = inspectorClipboard.rigidbody; changed = true; },
                [&]() { obj.rigidbody = inspectorClipboard.rigidbody; changed = true; },
                removeRigidbody);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Rigidbody3D");
            if (beginCompFields("##Fields_Rigidbody3D")) {
                noteRow("Collider required for physics.");
                /*if (UsesUIOnly2DPhysics(obj)) {
                    noteRow("Rigidbody3D is for 3D objects (use Rigidbody2D for UI/canvas).");
                }*/
                const float massUnitScale = std::max(0.000001f,
                    ProjectMassUnitToKilograms(projectManager.currentProject.physicsSettings.massUnit));
                const float minDisplayMass = std::max(0.0001f, 0.01f / massUnitScale);
                const std::string massLabel = std::string(Loc::Field("COMPONENT_RIGIDBODY3D", "mass", "Mass")) + " (" +
                    ProjectMassUnitSuffix(projectManager.currentProject.physicsSettings.massUnit) + ")";
                fieldRow(massLabel.c_str());
                if (ImGui::DragFloat("##Mass", &obj.rigidbody.mass, minDisplayMass * 0.05f, minDisplayMass, 1000000.0f, "%.3f")) {
                    obj.rigidbody.mass = std::max(minDisplayMass, obj.rigidbody.mass);
                    changed = true;
                }
                if (boolRow(Loc::Field("COMPONENT_RIGIDBODY3D", "useCustomCenterOfMass", "Custom Center Of Mass"), &obj.rigidbody.useCustomCenterOfMass)) { changed = true; }
                if (obj.rigidbody.useCustomCenterOfMass) {
                    fieldRow(Loc::Field("COMPONENT_RIGIDBODY3D", "centerOfMass", "Center Of Mass"));
                    if (ImGui::DragFloat3("##CenterOfMass", &obj.rigidbody.centerOfMass.x, 0.01f, -1000.0f, 1000.0f, "%.3f")) {
                        changed = true;
                    }
                }
                if (boolRow(Loc::Field("COMPONENT_RIGIDBODY3D", "useGravity", "Use Gravity"), &obj.rigidbody.useGravity)) { changed = true; }
                if (boolRow(Loc::Field("COMPONENT_RIGIDBODY3D", "isKinematic", "Kinematic"), &obj.rigidbody.isKinematic)) { changed = true; }
                fieldRow(Loc::Field("COMPONENT_RIGIDBODY3D", "linearDamping", "Linear Damp"));
                if (ImGui::DragFloat("##LinearDamping", &obj.rigidbody.linearDamping, 0.01f, 0.0f, 10.0f)) {
                    obj.rigidbody.linearDamping = std::clamp(obj.rigidbody.linearDamping, 0.0f, 10.0f);
                    changed = true;
                }
                fieldRow(Loc::Field("COMPONENT_RIGIDBODY3D", "angularDamping", "Angular Damp"));
                if (ImGui::DragFloat("##AngularDamping", &obj.rigidbody.angularDamping, 0.01f, 0.0f, 10.0f)) {
                    obj.rigidbody.angularDamping = std::clamp(obj.rigidbody.angularDamping, 0.0f, 10.0f);
                    changed = true;
                }
                fieldRow(Loc::Field("COMPONENT_RIGIDBODY3D", "interpolationMode", "Interpolation"));
                const char* interpolationModes[] = { "None", "Interpolate", "Extrapolate" };
                obj.rigidbody.interpolationMode = std::clamp(obj.rigidbody.interpolationMode, 0, 2);
                if (ImGui::Combo("##RigidbodyInterpolation", &obj.rigidbody.interpolationMode,
                                 interpolationModes, IM_ARRAYSIZE(interpolationModes))) {
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("None: show the latest physics pose.\n"
                                      "Interpolate: smooth between fixed steps with one-step latency.\n"
                                      "Extrapolate: predict forward from velocity for minimum visual latency.");
                }
                if (obj.rigidbody.interpolationMode == 2) {
                    fieldRow(Loc::Field("COMPONENT_RIGIDBODY3D", "maxExtrapolationTime", "Max Extrapolation"));
                    if (ImGui::DragFloat("##RigidbodyMaxExtrapolation", &obj.rigidbody.maxExtrapolationTime,
                                         0.001f, 0.0f, 0.1f, "%.3f s")) {
                        obj.rigidbody.maxExtrapolationTime =
                            std::clamp(obj.rigidbody.maxExtrapolationTime, 0.0f, 0.1f);
                        changed = true;
                    }
                }
                endCompFields();
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Rotation Constraints", nullptr, true)) {
                if (beginCompFields("##Fields_Rb3DConstraints")) {
                    if (boolRow(Loc::Field("COMPONENT_RIGIDBODY3D", "lockRotationX", "Lock X"), &obj.rigidbody.lockRotationX)) { changed = true; }
                    if (boolRow(Loc::Field("COMPONENT_RIGIDBODY3D", "lockRotationY", "Lock Y"), &obj.rigidbody.lockRotationY)) { changed = true; }
                    if (boolRow(Loc::Field("COMPONENT_RIGIDBODY3D", "lockRotationZ", "Lock Z"), &obj.rigidbody.lockRotationZ)) { changed = true; }
                    endCompFields();
                }
            }
            ImGui::PopID();
        }
        if (removeRigidbody) {
            obj.hasRigidbody = false;
            changed = true;
        }
        if (changed) {
            rigidbodySectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "rigidbody2d" && obj.hasRigidbody2D && sharedRigidbody2D) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.55f, 0.45f, 1.0f));
        bool removeRigidbody2D = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_RIGIDBODY2_D", "Rigidbody2D"), "Rigidbody2D", "rigidbody2d", &obj.rigidbody2D.enabled, true, [&]() {
            drawStandardComponentMenu(
                "rigidbody2d",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Rigidbody2D,
                [&]() { obj.rigidbody2D = Rigidbody2DComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::Rigidbody2D; inspectorClipboard.rigidbody2D = obj.rigidbody2D; },
                [&]() { obj.hasRigidbody2D = true; obj.rigidbody2D = inspectorClipboard.rigidbody2D; changed = true; },
                [&]() { obj.rigidbody2D = inspectorClipboard.rigidbody2D; changed = true; },
                removeRigidbody2D);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Rigidbody2D");
            if (beginCompFields("##Fields_Rigidbody2D")) {
                if (!UsesUIOnly2DPhysics(obj)) {
                    noteRow("Rigidbody2D is for UI/canvas objects only.");
                }
                if (boolRow(Loc::T("COMPONENT_RIGIDBODY2_D_USE_GRAVITY", "Use Gravity"), &obj.rigidbody2D.useGravity)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_RIGIDBODY2_D_GRAVITY_SCALE", "Gravity Scale"));
                if (ImGui::DragFloat("##GravityScale", &obj.rigidbody2D.gravityScale, 0.05f, 0.0f, 10.0f, "%.2f")) {
                    obj.rigidbody2D.gravityScale = std::max(0.0f, obj.rigidbody2D.gravityScale);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_RIGIDBODY2_D_LINEAR_DAMP", "Linear Damp"));
                if (ImGui::DragFloat("##LinearDamping", &obj.rigidbody2D.linearDamping, 0.01f, 0.0f, 10.0f)) {
                    obj.rigidbody2D.linearDamping = std::clamp(obj.rigidbody2D.linearDamping, 0.0f, 10.0f);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_RIGIDBODY2_D_VELOCITY", "Velocity"));
                if (ImGui::DragFloat2("##Velocity", &obj.rigidbody2D.velocity.x, 0.1f)) {
                    changed = true;
                }
                endCompFields();
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Rotation Constraints", nullptr, true)) {
                if (beginCompFields("##Fields_Rb2DConstraints")) {
                    noteRow("Locks the object's Z-axis rotation.");
                    if (boolRow(Loc::T("COMPONENT_RIGIDBODY2_D_LOCK_ROTATION", "Lock Rotation"), &obj.rigidbody2D.lockRotation)) { changed = true; }
                    endCompFields();
                }
            }
            ImGui::PopID();
        }
        if (removeRigidbody2D) {
            obj.hasRigidbody2D = false;
            changed = true;
        }
        if (changed) {
            rigidbody2DSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "collider2d" && obj.hasCollider2D && sharedCollider2D) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.5f, 0.65f, 1.0f));
        bool removeCollider2D = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_COLLIDER2_D", "Collider2D"), "Collider2D", "collider2d", &obj.collider2D.enabled, true, [&]() {
            drawStandardComponentMenu(
                "collider2d",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Collider2D,
                [&]() { obj.collider2D = Collider2DComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::Collider2D; inspectorClipboard.collider2D = obj.collider2D; },
                [&]() { obj.hasCollider2D = true; obj.collider2D = inspectorClipboard.collider2D; changed = true; },
                [&]() { obj.collider2D = inspectorClipboard.collider2D; changed = true; },
                removeCollider2D);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Collider2D");

            auto ensureHexagon = [&](Collider2DComponent& col, const glm::vec2& size) {
                if (!col.points.empty()) return;
                float radius = 0.5f * std::min(size.x, size.y);
                col.points.clear();
                for (int i = 0; i < 6; ++i) {
                    float ang = static_cast<float>(i) * (2.0f * PI / 6.0f);
                    col.points.emplace_back(std::cos(ang) * radius, std::sin(ang) * radius);
                }
            };
            auto ensureEdge = [&](Collider2DComponent& col, const glm::vec2& size) {
                if (col.points.size() >= 2) return;
                float half = size.x * 0.5f;
                col.points = { glm::vec2(-half, 0.0f), glm::vec2(half, 0.0f) };
            };

            if (beginCompFields("##Fields_Collider2D")) {
                if (!UsesUIOnly2DPhysics(obj)) {
                    noteRow("Collider2D is for UI/canvas objects only.");
                }
                if (boolRow(Loc::T("COMPONENT_COLLIDER2_D_IS_TRIGGER", "Is Trigger"), &obj.collider2D.isTrigger)) {
                    changed = true;
                }
                const char* colliderTypes[] = { "Box", "Polygon", "Edge" };
                int colliderType = static_cast<int>(obj.collider2D.type);
                fieldRow(Loc::T("COMPONENT_COLLIDER2_D_TYPE", "Type"));
                if (ImGui::Combo("##Type", &colliderType, colliderTypes, IM_ARRAYSIZE(colliderTypes))) {
                    obj.collider2D.type = static_cast<Collider2DType>(colliderType);
                    if (obj.collider2D.type == Collider2DType::Polygon) {
                        obj.collider2D.closed = true;
                    } else if (obj.collider2D.type == Collider2DType::Edge) {
                        obj.collider2D.closed = false;
                    }
                    changed = true;
                }

                if (obj.collider2D.type == Collider2DType::Box) {
                    fieldRow(Loc::T("COMPONENT_COLLIDER2_D_BOX_SIZE", "Box Size"));
                    if (ImGui::DragFloat2("##BoxSize", &obj.collider2D.boxSize.x, 0.1f, 0.01f, 10000.0f, "%.2f")) {
                        obj.collider2D.boxSize.x = std::max(0.01f, obj.collider2D.boxSize.x);
                        obj.collider2D.boxSize.y = std::max(0.01f, obj.collider2D.boxSize.y);
                        changed = true;
                    }
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1);
                    if (ImGui::SmallButton("Match UI Size")) {
                        obj.collider2D.boxSize = glm::max(obj.ui.size, glm::vec2(1.0f));
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_COLLIDER2_D_OFFSET", "Offset"));
                    if (ImGui::DragFloat2("##Offset", &obj.collider2D.offset.x, 0.1f, -10000.0f, 10000.0f, "%.2f")) {
                        changed = true;
                    }
                    endCompFields();
                } else if (obj.collider2D.type == Collider2DType::Polygon) {
                    ensureHexagon(obj.collider2D, glm::max(obj.ui.size, glm::vec2(1.0f)));
                    noteRow("Points (local space)");
                    endCompFields();
                    for (size_t i = 0; i < obj.collider2D.points.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::DragFloat2("##point", &obj.collider2D.points[i].x, 0.1f)) {
                            changed = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            obj.collider2D.points.erase(obj.collider2D.points.begin() + static_cast<long>(i));
                            changed = true;
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    if (ImGui::SmallButton("Add Point")) {
                        obj.collider2D.points.push_back(glm::vec2(0.0f));
                        changed = true;
                    }
                    if (beginCompFields("##Fields_Collider2DOffsetPoly")) {
                        fieldRow(Loc::T("COMPONENT_COLLIDER2_D_OFFSET_2", "Offset"));
                        if (ImGui::DragFloat2("##Offset", &obj.collider2D.offset.x, 0.1f, -10000.0f, 10000.0f, "%.2f")) {
                            changed = true;
                        }
                        endCompFields();
                    }
                } else if (obj.collider2D.type == Collider2DType::Edge) {
                    ensureEdge(obj.collider2D, glm::max(obj.ui.size, glm::vec2(1.0f)));
                    if (boolRow(Loc::T("COMPONENT_COLLIDER2_D_CLOSED_LOOP", "Closed Loop"), &obj.collider2D.closed)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_COLLIDER2_D_THICKNESS", "Thickness"));
                    if (ImGui::DragFloat("##Thickness", &obj.collider2D.edgeThickness, 0.01f, 0.01f, 10.0f, "%.2f")) {
                        obj.collider2D.edgeThickness = std::max(0.01f, obj.collider2D.edgeThickness);
                        changed = true;
                    }
                    noteRow("Points (local space)");
                    endCompFields();
                    for (size_t i = 0; i < obj.collider2D.points.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::DragFloat2("##edgepoint", &obj.collider2D.points[i].x, 0.1f)) {
                            changed = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            obj.collider2D.points.erase(obj.collider2D.points.begin() + static_cast<long>(i));
                            changed = true;
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    if (ImGui::SmallButton("Add Point")) {
                        obj.collider2D.points.push_back(glm::vec2(0.0f));
                        changed = true;
                    }
                    if (beginCompFields("##Fields_Collider2DOffsetEdge")) {
                        fieldRow(Loc::T("COMPONENT_COLLIDER2_D_OFFSET_3", "Offset"));
                        if (ImGui::DragFloat2("##Offset", &obj.collider2D.offset.x, 0.1f, -10000.0f, 10000.0f, "%.2f")) {
                            changed = true;
                        }
                        endCompFields();
                    }
                }
            }
            ImGui::PopID();
        }
        if (removeCollider2D) {
            obj.hasCollider2D = false;
            changed = true;
        }
        if (changed) {
            collider2DSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "assemblage" && obj.hasAssemblage && sharedAssemblage) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.38f, 0.58f, 1.0f));
        bool removeAssemblage = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_ASSEMBLAGE", "Assemblage"), "Assemblage", "assemblage", &obj.assemblage.enabled, true, [&]() {
            if (ImGui::MenuItem("Remove Component")) {
                removeAssemblage = true;
            }
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Assemblage");
            if (beginCompFields("##Fields_Assemblage")) {
                noteRow("Cells live in the linked .moduasm asset, not in this scene.");

                fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_ASSET", "Asset"));
                ImGui::TextUnformatted(obj.assemblage.assetPath.empty()
                                           ? "(none)"
                                           : obj.assemblage.assetPath.c_str());

                fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_ID", "Assemblage Id"));
                ImGui::TextDisabled("%s", obj.assemblage.assemblageId.c_str());

                if (boolRow(Loc::T("COMPONENT_ASSEMBLAGE_SHOW_GRID", "Show Grid"), &obj.assemblage.showGrid)) { changed = true; }
                if (boolRow(Loc::T("COMPONENT_ASSEMBLAGE_SNAP", "Snap To Grid"), &obj.assemblage.snapToGrid)) { changed = true; }

                fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_GRID_COLOR", "Grid Color"));
                if (ImGui::ColorEdit4("##AssemblageGridColor", &obj.assemblage.gridColor.x,
                                      ImGuiColorEditFlags_AlphaBar)) {
                    changed = true;
                }
                endCompFields();
            }

            if (AssemblageRuntime::Entry* entry = assemblageRuntime.acquire(obj.assemblage.assetPath)) {
                if (beginCompFields("##Fields_AssemblageGrid")) {
                    fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_CELL_SIZE", "Cell Size"));
                    if (ImGui::DragFloat2("##AssemblageCellSize", &entry->asset.cellSize.x, 0.01f, 0.001f, 1000.0f, "%.3f")) {
                        entry->asset.cellSize.x = std::max(0.001f, entry->asset.cellSize.x);
                        entry->asset.cellSize.y = std::max(0.001f, entry->asset.cellSize.y);
                        assemblageRuntime.markAssetDirty(obj.assemblage.assetPath);
                        assemblageRuntime.markUnsaved(obj.assemblage.assetPath, true);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_ORIGIN", "Grid Origin"));
                    if (ImGui::DragFloat2("##AssemblageOrigin", &entry->asset.origin.x, 0.01f, -100000.0f, 100000.0f, "%.3f")) {
                        assemblageRuntime.markAssetDirty(obj.assemblage.assetPath);
                        assemblageRuntime.markUnsaved(obj.assemblage.assetPath, true);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_CHUNK", "Chunk Size"));
                    ImGui::TextDisabled("%d cells", entry->asset.chunkSize);
                    fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYERS", "Layers"));
                    ImGui::TextDisabled("%d", static_cast<int>(entry->asset.layers.size()));
                    fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_TILESET", "Tileset"));
                    ImGui::TextDisabled("%s", entry->asset.tilesetPath.empty()
                                                  ? "(none)"
                                                  : entry->asset.tilesetPath.c_str());
                    endCompFields();
                }
                if (assemblageRuntime.hasUnsavedChanges(obj.assemblage.assetPath)) {
                    if (ImGui::Button("Save Assemblage")) {
                        std::string error;
                        if (!assemblageRuntime.save(obj.assemblage.assetPath, error)) {
                            addConsoleMessage("Failed to save the Assemblage: " + error,
                                              ConsoleMessageType::Error);
                        } else {
                            addConsoleMessage("Saved " + obj.assemblage.assetPath,
                                              ConsoleMessageType::Success);
                        }
                    }
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                                   "Assemblage asset could not be loaded.");
            }
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
        if (removeAssemblage) {
            obj.hasAssemblage = false;
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
    }

    if (inspectorComponentKey == "assemblage_layer" && obj.hasAssemblageLayer && sharedAssemblageLayer) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.38f, 0.58f, 1.0f));
        bool removeLayer = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_ASSEMBLAGE_LAYER", "Assemblage Layer"), "AssemblageLayer", "assemblage_layer", &obj.assemblageLayer.enabled, true, [&]() {
            if (ImGui::MenuItem("Remove Component")) {
                removeLayer = true;
            }
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("AssemblageLayer");
            if (beginCompFields("##Fields_AssemblageLayer")) {
                fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYER_ID", "Layer Id"));
                ImGui::TextDisabled("%d", obj.assemblageLayer.layerId);

                fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYER_SORTING", "Sorting Order"));
                if (ImGui::DragInt("##AssemblageLayerSorting", &obj.assemblageLayer.sortingOrder, 0.2f, -4096, 4096)) {
                    changed = true;
                }
                noteRow("Sprites with an order between two layers draw between them.");

                if (boolRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYER_LOCKED", "Locked"), &obj.assemblageLayer.locked)) { changed = true; }

                fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYER_OPACITY", "Opacity"));
                if (ImGui::SliderFloat("##AssemblageLayerOpacity", &obj.assemblageLayer.opacity, 0.0f, 1.0f, "%.2f")) {
                    changed = true;
                }

                fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYER_TINT", "Tint"));
                if (ImGui::ColorEdit4("##AssemblageLayerTint", &obj.assemblageLayer.tint.x, ImGuiColorEditFlags_AlphaBar)) {
                    changed = true;
                }

                if (boolRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYER_LIT", "Receive Lighting 2D"), &obj.assemblageLayer.receiveLighting2D)) { changed = true; }
                if (boolRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYER_UNLIT", "Force Unlit"), &obj.assemblageLayer.unlitLighting2D)) { changed = true; }

                fieldRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYER_EMISSIVE", "Emissive"));
                if (ImGui::DragFloat("##AssemblageLayerEmissive", &obj.assemblageLayer.emissiveLighting2D, 0.01f, 0.0f, 8.0f, "%.2f")) {
                    changed = true;
                }

                if (boolRow(Loc::T("COMPONENT_ASSEMBLAGE_LAYER_COLLISION", "Collision"), &obj.assemblageLayer.collisionEnabled)) { changed = true; }
                noteRow("Tile collision is generated in a later stage.");
                endCompFields();
            }
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
        if (removeLayer) {
            obj.hasAssemblageLayer = false;
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
    }

    if (inspectorComponentKey == "parallax2d" && obj.hasParallaxLayer2D && sharedParallax2D) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.28f, 0.45f, 0.6f, 1.0f));
        bool removeParallax = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_PARALLAX_LAYER2_D", "Parallax Layer 2D"), "ParallaxLayer2D", "parallax2d", &obj.parallaxLayer2D.enabled, true, [&]() {
            drawStandardComponentMenu(
                "parallax2d",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Parallax2D,
                [&]() { obj.parallaxLayer2D = ParallaxLayer2DComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::Parallax2D; inspectorClipboard.parallax2D = obj.parallaxLayer2D; },
                [&]() { obj.hasParallaxLayer2D = true; obj.parallaxLayer2D = inspectorClipboard.parallax2D; changed = true; },
                [&]() { obj.parallaxLayer2D = inspectorClipboard.parallax2D; changed = true; },
                removeParallax);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("ParallaxLayer2D");
            if (beginCompFields("##Fields_Parallax2D")) {
                if (!isUIObject(obj)) {
                    noteRow("Parallax layers are for UI world objects.");
                }
                fieldRow(Loc::T("COMPONENT_PARALLAX_LAYER2_D_ORDER", "Order"));
                if (ImGui::DragInt("##Order", &obj.parallaxLayer2D.order, 1.0f)) {
                    changed = true;
                }
                int lowerCount = 0;
                int higherCount = 0;
                for (const auto& other : sceneObjects) {
                    if (other.id == obj.id || !other.enabled) continue;
                    if (!other.hasParallaxLayer2D || !other.parallaxLayer2D.enabled) continue;
                    if (!isUIObject(other)) continue;
                    if (other.parallaxLayer2D.order < obj.parallaxLayer2D.order) ++lowerCount;
                    if (other.parallaxLayer2D.order > obj.parallaxLayer2D.order) ++higherCount;
                }
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Layer stack: %d behind, %d in front", lowerCount, higherCount);
                fieldRow(Loc::T("COMPONENT_PARALLAX_LAYER2_D_PARALLAX_FACTOR", "Parallax Factor"));
                if (ImGui::DragFloat("##ParallaxFactor", &obj.parallaxLayer2D.factor, 0.01f, 0.0f, 1.0f, "%.2f")) {
                    obj.parallaxLayer2D.factor = std::clamp(obj.parallaxLayer2D.factor, 0.0f, 1.0f);
                    changed = true;
                }
                if (HorizontalBoolRow("Repeat", "X", &obj.parallaxLayer2D.repeatX, "Y", &obj.parallaxLayer2D.repeatY))
                {
                    changed = true;
                }
                if (boolRow(Loc::T("COMPONENT_PARALLAX_LAYER2_D_DISABLE_CULLING", "Disable Culling"), &obj.parallaxLayer2D.disableCulling)) { changed = true; }

                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
                    ImGui::TextUnformatted("Prevents this parallax object from being culled when it moves outside the visible world overlay.");
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
                fieldRow(Loc::T("COMPONENT_PARALLAX_LAYER2_D_REPEAT_SPACING", "Repeat Spacing"));
                if (ImGui::DragFloat2("##RepeatSpacing", &obj.parallaxLayer2D.repeatSpacing.x, 0.1f, 0.0f, 10000.0f, "%.1f")) {
                    obj.parallaxLayer2D.repeatSpacing.x = std::max(0.0f, obj.parallaxLayer2D.repeatSpacing.x);
                    obj.parallaxLayer2D.repeatSpacing.y = std::max(0.0f, obj.parallaxLayer2D.repeatSpacing.y);
                    changed = true;
                }
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeParallax) {
            obj.hasParallaxLayer2D = false;
            changed = true;
        }
        if (changed) {
            parallax2DSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "audio_source" && obj.hasAudioSource && sharedAudioSource) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.55f, 0.4f, 0.3f, 1.0f));
        bool removeAudioSource = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_AUDIO_SOURCE", "Audio Source"), "AudioSource", "audio_source", &obj.audioSource.enabled, true, [&]() {
            if (ImGui::MenuItem("Reset Component Values")) {
                obj.audioSource = AudioSourceComponent{};
                changed = true;
            }
            if (ImGui::MenuItem("Add Local Reverb Zone Component", nullptr, false, !obj.hasReverbZone)) {
                obj.hasReverbZone = true;
                obj.reverbZone = ReverbZoneComponent{};
                changed = true;
            }
            drawClipboardMenus(
                "Copy Audio Source Values",
                "Paste Audio Source Values as New",
                "Paste Audio Source Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::AudioSource,
                [&]() {
                    inspectorClipboard.kind = InspectorClipboardKind::AudioSource;
                    inspectorClipboard.audioSource = obj.audioSource;
                },
                [&]() {
                    obj.hasAudioSource = true;
                    obj.audioSource = inspectorClipboard.audioSource;
                    changed = true;
                },
                [&]() {
                    obj.audioSource = inspectorClipboard.audioSource;
                    changed = true;
                });
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeAudioSource = true;
            }
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("AudioSource");
            auto& src = obj.audioSource;
            const std::string previousClipPath = src.clipPath;

            if (drawAssetReferenceSlot("##AudioClipSlot", "Sound Clip", src.clipPath, FileCategory::Audio, "Sound Clip")) {
                if (previousClipPath != src.clipPath && audio.isPreviewing(previousClipPath)) {
                    stopClipPreview();
                }
                changed = true;
            }

            if (!src.clipPath.empty()) {
                drawTrimmedPathText(src.clipPath, ImVec4(0.78f, 0.88f, 1.0f, 1.0f));
            }

            const bool usePlanar2DAudio = isProject2DPipeline() || HasUIComponent(obj);
            const char* spatialBlendLabel = usePlanar2DAudio ? "Localization" : "Spatial Blend";
            const char* minDistanceLabel = usePlanar2DAudio ? "Near Distance" : "Min Distance";
            const char* maxDistanceLabel = usePlanar2DAudio ? "Far Distance" : "Max Distance";

            if (beginCompFields("##Fields_AudioSource")) {
                fieldRow(Loc::T("COMPONENT_AUDIO_SOURCE_VOLUME", "Volume"));
                if (ImGui::SliderFloat("##Volume", &src.volume, 0.0f, 1.5f, "%.2f")) {
                    changed = true;
                    syncClipPreviewVolume(AudioPreviewContext::AudioSourceComponent, src.volume);
                }
                if (boolRow(Loc::T("COMPONENT_AUDIO_SOURCE_LOOP", "Loop"), &src.loop)) { changed = true; }
                if (boolRow(Loc::T("COMPONENT_AUDIO_SOURCE_PLAY_ON_START", "Play On Start"), &src.playOnStart)) { changed = true; }
                fieldRow(spatialBlendLabel);
                if (ImGui::SliderFloat("##SpatialBlend", &src.spatialBlend, 0.0f, 1.0f, "%.2f")) {
                    src.spatialBlend = std::clamp(src.spatialBlend, 0.0f, 1.0f);
                    src.spatial = src.spatialBlend > 0.001f;
                    changed = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
                    ImGui::TextUnformatted(usePlanar2DAudio
                    ? "0 keeps audio global and centered. 1 fully uses world position for pan and falloff."
                    : "0 keeps audio centered. 1 uses full world placement.");
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1);
                ImGui::BeginDisabled(src.spatialBlend <= 0.001f);
                fieldRow(minDistanceLabel);
                if (ImGui::DragFloat("##MinDist", &src.minDistance, 0.1f, 0.1f, 200.0f, "%.2f")) {
                    src.minDistance = std::max(0.1f, src.minDistance);
                    changed = true;
                }
                fieldRow(maxDistanceLabel);
                if (ImGui::DragFloat("##MaxDist", &src.maxDistance, 0.1f, src.minDistance + 0.5f, 500.0f, "%.2f")) {
                    src.maxDistance = std::max(src.maxDistance, src.minDistance + 0.5f);
                    changed = true;
                }
                if (!usePlanar2DAudio) {
                    const char* rolloffModes[] = { "Logarithmic", "Linear", "Exponential", "Custom" };
                    int rolloffIndex = static_cast<int>(src.rolloffMode);
                    fieldRow(Loc::T("COMPONENT_AUDIO_SOURCE_ROLLOFF_MODE", "Rolloff Mode"));
                    if (ImGui::Combo("##RolloffMode", &rolloffIndex, rolloffModes, IM_ARRAYSIZE(rolloffModes))) {
                        src.rolloffMode = static_cast<AudioRolloffMode>(rolloffIndex);
                        changed = true;
                    }
                    if (src.rolloffMode != AudioRolloffMode::Custom) {
                        fieldRow(Loc::T("COMPONENT_AUDIO_SOURCE_ROLLOFF_FACTOR", "Rolloff Factor"));
                        if (ImGui::SliderFloat("##RolloffFactor", &src.rolloff, 0.1f, 4.0f, "%.2f")) {
                            src.rolloff = std::max(0.1f, src.rolloff);
                            changed = true;
                        }
                    } else {
                        fieldRow(Loc::T("COMPONENT_AUDIO_SOURCE_MID_DISTANCE", "Mid Distance"));
                        if (ImGui::SliderFloat("##MidDist", &src.customMidDistance, 0.0f, 1.0f, "%.2f")) {
                            src.customMidDistance = std::clamp(src.customMidDistance, 0.0f, 1.0f);
                            changed = true;
                        }
                        fieldRow(Loc::T("COMPONENT_AUDIO_SOURCE_MID_GAIN", "Mid Gain"));
                        if (ImGui::SliderFloat("##MidGain", &src.customMidGain, 0.0f, 1.0f, "%.2f")) {
                            src.customMidGain = std::clamp(src.customMidGain, 0.0f, 1.0f);
                            changed = true;
                        }
                        fieldRow(Loc::T("COMPONENT_AUDIO_SOURCE_END_GAIN", "End Gain"));
                        if (ImGui::SliderFloat("##EndGain", &src.customEndGain, 0.0f, 1.0f, "%.2f")) {
                            src.customEndGain = std::clamp(src.customEndGain, 0.0f, 1.0f);
                            changed = true;
                        }
                    }
                } else {
                    noteRow("2D projects use planar left/right placement with the blend slider.");
                }
                ImGui::EndDisabled();
                endCompFields();
            }

            const AudioClipPreview* clipPreview = audio.getPreview(src.clipPath);
            ImGui::Separator();
            const bool previewPlaying = !src.clipPath.empty() && audio.isPreviewing(src.clipPath);
            drawAudioCenteredText(
                src.clipPath.empty() ? "No clip selected" : fs::path(src.clipPath).filename().string(),
                nullptr,
                src.clipPath.empty() ? nullptr : src.clipPath.c_str());
            ImGui::Spacing();

            const float sourceButton = 44.0f;
            const float sourceRow = sourceButton * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                + std::max(0.0f, (ImGui::GetContentRegionAvail().x - sourceRow) * 0.5f));

            if (drawAudioPlayerIconButton(
                    "##AudioSourceLoopButton",
                    src.loop
                        ? "Resources/Engine-Root/Audio Player/Loop Toggled On.png"
                        : "Resources/Engine-Root/Audio Player/Loop Toggled Off.png",
                    "Loop",
                    src.loop ? "Disable loop" : "Enable loop",
                    src.loop,
                    src.clipPath.empty(),
                    ImVec2(sourceButton, sourceButton),
                    ImVec4(0.42f, 0.76f, 1.0f, 1.0f))) {
                src.loop = !src.loop;
                changed = true;
                if (previewPlaying) {
                    audio.setPreviewLoop(src.loop);
                }
            }
            ImGui::SameLine();
            if (drawAudioPlayerIconButton(
                    "##AudioSourcePlayButton",
                    previewPlaying
                        ? "Resources/Engine-Root/Audio Player/Play Button Toggled On.png"
                        : "Resources/Engine-Root/Audio Player/Play Button Toggled Off.png",
                    "Play",
                    previewPlaying ? "Stop preview" : "Play preview",
                    previewPlaying,
                    src.clipPath.empty(),
                    ImVec2(sourceButton, sourceButton),
                    ImVec4(0.92f, 0.55f, 0.30f, 1.0f))) {
                if (previewPlaying) {
                    stopClipPreview();
                } else {
                    beginClipPreview(src.clipPath, src.volume, src.loop, AudioPreviewContext::AudioSourceComponent);
                }
            }

            drawAudioPreviewVolumeControl("##AudioSourcePreviewVolume", AudioPreviewContext::AudioSourceComponent, src.volume);

            if (clipPreview) {
                char sourceFormatLine[96] = {};
                std::snprintf(sourceFormatLine, sizeof(sourceFormatLine), "%u channels  |  %u Hz",
                    clipPreview->channels,
                    clipPreview->sampleRate);
                ImGui::Spacing();
                drawAudioCenteredText(sourceFormatLine, nullptr);
            } else {
                ImGui::Spacing();
                drawAudioCenteredText("Load an audio clip to preview timing and waveform.", nullptr);
            }

            ImVec2 waveSize(ImGui::GetContentRegionAvail().x, 72.0f);
            double cur = 0.0;
            double dur = clipPreview ? clipPreview->durationSeconds : 0.0;
            float progress = -1.0f;
            if (audio.getPreviewTime(src.clipPath, cur, dur) && dur > 0.0001) {
                progress = static_cast<float>(cur / dur);
            }
            float seekRatio = -1.0f;
            drawWaveform("##AudioWaveComponent", clipPreview, waveSize, progress, &seekRatio);
            if (seekRatio >= 0.0f && dur > 0.0) {
                audio.seekPreview(src.clipPath, seekRatio * dur);
            }

            drawAudioTimeReadout(cur, dur);

            ImGui::PopID();
        }
        if (removeAudioSource) {
            if (audio.isPreviewing(obj.audioSource.clipPath)) {
                stopClipPreview();
            }
            obj.hasAudioSource = false;
            changed = true;
        }
        if (changed) {
            audioSourceSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "audio_fx" && obj.hasAudioFX && sharedAudioFX) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.36f, 0.5f, 0.62f, 1.0f));
        bool changed = false;
        bool removeAudioFX = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_AUDIO_FX", "Audio FX"), "AudioFX", "audio_fx", &obj.audioFX.enabled, true, [&]() {
            drawStandardComponentMenu(
                "audio_fx",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::AudioFX,
                [&]() { obj.audioFX = AudioFXComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::AudioFX; inspectorClipboard.audioFX = obj.audioFX; },
                [&]() { obj.hasAudioFX = true; obj.audioFX = inspectorClipboard.audioFX; changed = true; },
                [&]() { obj.audioFX = inspectorClipboard.audioFX; changed = true; },
                removeAudioFX);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("AudioFX");
            AudioFXChain& fx = obj.audioFX.chain;

            if (ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_FX_APPLY_TO_ALL_SOURCES_GLOBAL", "Apply to All Sources (Global)"), &obj.audioFX.global)) changed = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When on, this chain drives the global audio bus and\n"
                                  "affects every audio source instead of just this object.");
            }
            if (!obj.audioFX.global && !obj.hasAudioSource) {
                ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.35f, 1.0f),
                                   "Add an Audio Source, or enable Global, for these effects to be heard.");
            }

            const std::vector<std::string>& presetNames = AudioFXPresets::names();
            std::string currentPreset = fx.name.empty() ? std::string("None") : fx.name;
            if (ImGui::BeginCombo("Preset", currentPreset.c_str())) {
                for (const std::string& name : presetNames) {
                    bool selected = (currentPreset == name);
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        fx = AudioFXPresets::get(name);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_STEREO_WIDTH", "Stereo Width"), &fx.stereoWidth, 0.0f, 1.0f, "%.2f")) changed = true;

            int removeIndex = -1;
            for (size_t i = 0; i < fx.effects.size(); ++i) {
                AudioFXEffect& e = fx.effects[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Separator();

                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::BeginCombo("##fxtype", AudioFXTypeName(e.type))) {
                    for (int t = 0; t < static_cast<int>(AudioFXType::Count); ++t) {
                        bool selected = (t == static_cast<int>(e.type));
                        if (ImGui::Selectable(AudioFXTypeName(static_cast<AudioFXType>(t)), selected)) {
                            e.type = static_cast<AudioFXType>(t);
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_FX_ON_FX", "On##fx"), &e.enabled)) changed = true;
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) removeIndex = static_cast<int>(i);

                switch (e.type) {
                    case AudioFXType::LowPass:
                    case AudioFXType::HighPass:
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_CUTOFF_HZ", "Cutoff (Hz)"), &e.freq, 20.0f, 20000.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_RESONANCE", "Resonance"), &e.q, 0.1f, 8.0f, "%.2f")) changed = true;
                        break;
                    case AudioFXType::BassBoost:
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_FREQ_HZ", "Freq (Hz)"), &e.freq, 40.0f, 400.0f, "%.0f")) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_GAIN_D_B", "Gain (dB)"), &e.gainDb, 0.0f, 18.0f, "%.1f")) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_LOOSENESS", "Looseness"), &e.param2, 0.0f, 1.0f, "%.2f")) changed = true;
                        break;
                    case AudioFXType::Distortion:
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_DRIVE", "Drive"), &e.amount, 0.0f, 1.0f, "%.2f")) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_TONE", "Tone"), &e.param1, 0.0f, 1.0f, "%.2f")) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_MIX", "Mix"), &e.mix, 0.0f, 1.0f, "%.2f")) changed = true;
                        break;
                    case AudioFXType::Reverb:
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_ROOM", "Room"), &e.param1, 0.0f, 1.0f, "%.2f")) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_DAMPING", "Damping"), &e.param2, 0.0f, 1.0f, "%.2f")) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_MIX_2", "Mix"), &e.mix, 0.0f, 1.0f, "%.2f")) changed = true;
                        break;
                    case AudioFXType::Echo:
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_TIME_S", "Time (s)"), &e.param1, 0.01f, 1.5f, "%.3f")) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_FEEDBACK", "Feedback"), &e.param2, 0.0f, 0.95f, "%.2f")) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_MIX_3", "Mix"), &e.mix, 0.0f, 1.0f, "%.2f")) changed = true;
                        break;
                    case AudioFXType::Pitch:
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_SEMITONES", "Semitones"), &e.param3, -24.0f, 24.0f, "%.1f")) changed = true;
                        break;
                    case AudioFXType::WowFlutter:
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_RATE_HZ", "Rate (Hz)"), &e.param1, 0.1f, 12.0f, "%.2f")) changed = true;
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_DEPTH", "Depth"), &e.amount, 0.0f, 1.0f, "%.2f")) changed = true;
                        break;
                    case AudioFXType::NoiseHiss:
                        if (ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_FX_LEVEL", "Level"), &e.amount, 0.0f, 1.0f, "%.2f")) changed = true;
                        break;
                    default:
                        break;
                }
                ImGui::PopID();
            }

            if (removeIndex >= 0 && removeIndex < static_cast<int>(fx.effects.size())) {
                fx.effects.erase(fx.effects.begin() + removeIndex);
                changed = true;
            }

            ImGui::Separator();
            if (ImGui::Button("+ Add Effect") && fx.effects.size() < static_cast<size_t>(AudioFXProcessor::kMaxEffects)) {
                fx.effects.push_back(AudioFXEffect{});
                if (fx.name.empty()) fx.name = "Custom";
                changed = true;
            }
            ImGui::PopID();
        }
        if (removeAudioFX) {
            obj.hasAudioFX = false;
            changed = true;
        }
        if (changed) {
            audioFXSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "video_player" && obj.hasVideoPlayer && sharedVideoPlayer) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.34f, 0.38f, 0.60f, 1.0f));
        bool removeVideoPlayer = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_VIDEO_PLAYER", "Video Player"), "VideoPlayer", "video_player", &obj.videoPlayer.enabled, true, [&]() {
            if (ImGui::MenuItem("Reset Component Values")) {
                obj.videoPlayer = VideoPlayerComponent{};
                changed = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeVideoPlayer = true;
            }
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("VideoPlayer");
            auto& player = obj.videoPlayer;

            // Derive audio mode from the two internal bools for the dropdown.
            // 0 = Disabled, 1 = Direct, 2 = Audio Source
            int audioMode = !player.playAudioFromVideo ? 0
                          : player.routeAudioToSource  ? 2
                                                       : 1;

            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            if (ImGui::BeginTabBar("##VideoPlayerTabs")) {

                if (ImGui::BeginTabItem("Playback")) {
                    ImGui::Dummy(ImVec2(0.0f, 1.0f));
                    if (beginCompFields("##VP_Playback")) {
                        fieldRow(Loc::T("COMPONENT_VIDEO_PLAYER_VIDEO", "Video"));
                        if (drawAssetReferenceSlot("##VideoPath", nullptr, player.videoPath,
                                                   FileCategory::Video, "Video Clip")) {
                            changed = true;
                        }
                        if (boolRow(Loc::T("COMPONENT_VIDEO_PLAYER_PLAY_ON_AWAKE", "Play On Awake"), &player.playOnAwake)) { changed = true; }
                        if (boolRow(Loc::T("COMPONENT_VIDEO_PLAYER_LOOP", "Loop"), &player.loop))                  { changed = true; }
                        if (boolRow(Loc::T("COMPONENT_VIDEO_PLAYER_FLIP_X", "Flip X"), &player.flipX))               { changed = true; }
                        if (boolRow(Loc::T("COMPONENT_VIDEO_PLAYER_FLIP_Y", "Flip Y"), &player.flipY))               { changed = true; }
                        fieldRow(Loc::T("COMPONENT_VIDEO_PLAYER_SPEED", "Speed"));
                        if (ImGui::DragFloat("##VideoPlaybackSpeed", &player.playbackSpeed, 0.01f, 0.0f, 8.0f, "%.2fx")) {
                            player.playbackSpeed = std::clamp(player.playbackSpeed, 0.0f, 8.0f);
                            changed = true;
                        }
                        endCompFields();
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Output")) {
                    ImGui::Dummy(ImVec2(0.0f, 1.0f));
                    ImGui::TextDisabled("Audio");
                    ImGui::Spacing();
                    if (beginCompFields("##VP_Audio")) {
                        fieldRow(Loc::T("COMPONENT_VIDEO_PLAYER_AUDIO_MODE", "Audio Mode"));
                        const char* audioModeNames[] = { "Disabled", "Direct", "Audio Source" };
                        ImGui::PushID("AudioMode");
                        if (ImGui::BeginCombo("##AudioModeCombo", audioModeNames[audioMode])) {
                            for (int i = 0; i < 3; ++i) {
                                const bool sel = (audioMode == i);
                                if (ImGui::Selectable(audioModeNames[i], sel)) {
                                    audioMode = i;
                                    player.playAudioFromVideo  = (i != 0);
                                    player.routeAudioToSource  = (i == 2);
                                    changed = true;
                                }
                                if (sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::PopID();

                        if (audioMode == 2) {
                            fieldRow(Loc::T("COMPONENT_AUDIO_MODE_AUDIO_SOURCE", "Audio Source"));
                            // Empty falls back to the video player's own audio output.
                            if (drawObjectReferenceSlot("##VideoOutputAudioSourceRef",
                                                        nullptr,
                                                        player.outputAudioSourceObjectId,
                                                        -1,
                                                        "Audio Source")) {
                                changed = true;
                            }
                        }

                        if (audioMode != 0) {
                            fieldRow(Loc::T("COMPONENT_AUDIO_MODE_VOLUME", "Volume"));
                            if (ImGui::DragFloat("##VideoAudioVolume", &player.videoAudioVolume, 0.01f, 0.0f, 2.0f, "%.2f")) {
                                player.videoAudioVolume = std::clamp(player.videoAudioVolume, 0.0f, 2.0f);
                                changed = true;
                            }
                            if (boolRow(Loc::T("COMPONENT_AUDIO_MODE_MUTE", "Mute"), &player.videoAudioMuted)) { changed = true; }
                        }
                        endCompFields();
                    }

                    ImGui::Spacing();
                    if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Advanced")) {
                        ImGui::Spacing();
                        if (beginCompFields("##VP_Advanced")) {
                            if (boolRow(Loc::T("COMPONENT_AUDIO_MODE_KEEP_AUDIO_SYNCED", "Keep Audio Synced"), &player.syncAudioToVideo)) { changed = true; }
                            if (player.syncAudioToVideo) {
                                fieldRow(Loc::T("COMPONENT_AUDIO_MODE_SYNC_TOLERANCE", "Sync Tolerance"));
                                if (ImGui::DragFloat("##VideoAudioSyncTolerance", &player.audioSyncTolerance, 0.005f, 0.0f, 0.5f, "%.3f s")) {
                                    player.audioSyncTolerance = std::clamp(player.audioSyncTolerance, 0.0f, 0.5f);
                                    changed = true;
                                }
                            }
                            endCompFields();
                        }
                        ImGui::Spacing();
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::Dummy(ImVec2(0.0f, 1.0f));

            ImGui::PopID();
        }
        if (removeVideoPlayer) {
            obj.hasVideoPlayer = false;
            obj.videoPlayer = VideoPlayerComponent{};
            changed = true;
        }
        if (changed) {
            videoPlayerSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "network_manager" && obj.hasNetworkManager) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.34f, 0.38f, 0.60f, 1.0f));
        bool removeNetworkManager = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_NETWORK_MANAGER", "Network Manager"),
                                          "NetworkManager", "network_manager",
                                          &obj.networkManager.enabled, true, [&]() {
            if (ImGui::MenuItem("Reset Component Values")) {
                obj.networkManager = NetworkManagerComponent{};
                changed = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeNetworkManager = true;
            }
        }, iconCompNetwork);
        if (header.enabledChanged) changed = true;
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("NetworkManager");
            auto& mgr = obj.networkManager;

            const int managerCount = CountNetworkManagers(sceneObjects);
            if (managerCount > 1) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
                ImGui::TextWrapped("%d active Network Managers in this scene. Only one is allowed; disable or remove the others.",
                                   managerCount);
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            auto textRow = [&](const char* label, std::string& value, const char* id) {
                fieldRow(label);
                char buf[512] = {};
                std::snprintf(buf, sizeof(buf), "%s", value.c_str());
                if (ImGui::InputText(id, buf, sizeof(buf))) {
                    value = buf;
                    changed = true;
                }
            };

            if (beginCompFields("##NetManagerFields")) {
                textRow("App ID", mgr.appId, "##NetAppId");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Photon Realtime App ID. This is a credential:\n"
                                      "prefer project settings for a project you ship publicly.");
                }
                textRow("App Version", mgr.appVersion, "##NetAppVersion");
                textRow("Region", mgr.region, "##NetRegion");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Photon region code. Empty = automatic best ping.");
                }
                textRow("Nickname", mgr.nickname, "##NetNickname");
                textRow("Default Room", mgr.defaultRoomName, "##NetDefaultRoom");

                if (boolRow("Offline Mode", &mgr.offlineMode)) changed = true;
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Local single-player session with no transport.\n"
                                      "Gameplay code paths stay identical to online.");
                }
                if (boolRow("Auto Connect", &mgr.autoConnect)) changed = true;
                if (boolRow("Auto Join Lobby", &mgr.autoJoinLobby)) changed = true;

                fieldRow("Max Players");
                if (ImGui::DragInt("##NetMaxPlayers", &mgr.maxPlayers, 1.0f, 0, 255)) {
                    mgr.maxPlayers = std::clamp(mgr.maxPlayers, 0, 255);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 uses the backend default.");

                fieldRow("Send Rate (Hz)");
                if (ImGui::DragInt("##NetSendRate", &mgr.sendRateHz, 1.0f, 1, 120)) {
                    mgr.sendRateHz = std::clamp(mgr.sendRateHz, 1, 120);
                    changed = true;
                }
                fieldRow("Serialization Rate (Hz)");
                if (ImGui::DragInt("##NetSerRate", &mgr.serializationRateHz, 1.0f, 1, 120)) {
                    mgr.serializationRateHz = std::clamp(mgr.serializationRateHz, 1, 120);
                    changed = true;
                }
                fieldRow("Max Outbound B/s");
                if (ImGui::DragInt("##NetMaxOut", &mgr.maxOutboundBytesPerSecond, 64.0f, 0, 1000000)) {
                    mgr.maxOutboundBytesPerSecond = std::max(0, mgr.maxOutboundBytesPerSecond);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Outbound byte budget per second. 0 = unlimited.\n"
                                      "Updates over budget are dropped, not queued.");
                }
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeNetworkManager) {
            obj.hasNetworkManager = false;
            obj.networkManager = NetworkManagerComponent{};
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "network_identity" && obj.hasNetworkIdentity) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.34f, 0.38f, 0.60f, 1.0f));
        bool removeNetworkIdentity = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_NETWORK_IDENTITY", "Network Identity"),
                                          "NetworkIdentity", "network_identity",
                                          &obj.networkIdentity.enabled, true, [&]() {
            if (ImGui::MenuItem("Reset Component Values")) {
                obj.networkIdentity = NetworkIdentityComponent{};
                changed = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeNetworkIdentity = true;
            }
        }, iconCompNetwork);
        if (header.enabledChanged) changed = true;
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("NetworkIdentity");
            auto& net = obj.networkIdentity;

            if (CountNetworkManagers(sceneObjects) == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
                ImGui::TextWrapped("No Network Manager in this scene. This object will not replicate until one exists.");
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            if (beginCompFields("##NetIdentityFields")) {
                fieldRow("ModuOBJ Asset ID");
                {
                    char buf[256] = {};
                    std::snprintf(buf, sizeof(buf), "%s", net.assetId.c_str());
                    if (ImGui::InputText("##NetAssetId", buf, sizeof(buf))) {
                        net.assetId = buf;
                        changed = true;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Source ModuOBJ asset used to respawn this object on\n"
                                      "remote clients. Empty = scene-placed object.");
                }

                if (boolRow("Sync Position", &net.syncPosition)) changed = true;
                if (boolRow("Sync Rotation", &net.syncRotation)) changed = true;
                if (boolRow("Sync Scale", &net.syncScale)) changed = true;
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Off by default: scale rarely changes and costs bandwidth.");
                }
                if (boolRow("Sync Velocity", &net.syncVelocity)) changed = true;
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Send velocity so remote clients can extrapolate\n"
                                      "through brief packet loss.");
                }

                fieldRow("Sync Mode");
                {
                    const char* modes[] = { "Snapshot", "Interpolate", "Extrapolate" };
                    int mode = std::clamp(net.syncMode, 0, 2);
                    if (ImGui::Combo("##NetSyncMode", &mode, modes, 3)) {
                        net.syncMode = mode;
                        changed = true;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Snapshot: snap to the newest sample.\n"
                                      "Interpolate: render slightly behind and blend.\n"
                                      "Extrapolate: also project forward using velocity.");
                }

                fieldRow("Send Rate (Hz)");
                if (ImGui::DragInt("##NetIdSendRate", &net.sendRateHz, 1.0f, 0, 120)) {
                    net.sendRateHz = std::clamp(net.sendRateHz, 0, 120);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("0 uses the Network Manager's serialization rate.");
                }

                fieldRow("Interpolation Delay");
                if (ImGui::DragFloat("##NetInterpDelay", &net.interpolationDelay, 0.005f, 0.0f, 1.0f, "%.3f s")) {
                    net.interpolationDelay = std::clamp(net.interpolationDelay, 0.0f, 1.0f);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("How far behind the newest snapshot to render.\n"
                                      "Must cover normal snapshot spacing or interpolation runs dry.");
                }

                fieldRow("Max Extrapolation");
                if (ImGui::DragFloat("##NetMaxExtrap", &net.maxExtrapolation, 0.01f, 0.0f, 2.0f, "%.2f s")) {
                    net.maxExtrapolation = std::clamp(net.maxExtrapolation, 0.0f, 2.0f);
                    changed = true;
                }

                if (boolRow("Owner Only Writes", &net.ownerOnlyWrites)) changed = true;
                if (boolRow("Spawner Owns", &net.spawnerOwns)) changed = true;
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeNetworkIdentity) {
            obj.hasNetworkIdentity = false;
            obj.networkIdentity = NetworkIdentityComponent{};
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "particle_system2d" && obj.hasParticleSystem2D && sharedParticleSystem2D) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_PARTICLE_SYSTEM2_D", "Particle System"), "ParticleSystem2D", "particle_system2d", &obj.particleSystem2D.enabled, true, [&]() {
            if (ImGui::MenuItem("Reset")) {
                obj.particleSystem2D = ParticleSystem2DComponent{};
                changed = true;
            }
        });
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ParticleSystem2DComponent& ps = obj.particleSystem2D;
            auto minMaxRow = [&](const char* label, ParticleSystem2DComponent::MinMaxFloat& range) {
                ImGui::PushID(label);
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_RANDOM", "Random"), &range.random);
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_MIN", "Min"), &range.min, 0.01f, 0.0f, 100000.0f, "%.3f");
                if (range.random) {
                    changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_MAX", "Max"), &range.max, 0.01f, 0.0f, 100000.0f, "%.3f");
                    if (range.max < range.min) range.max = range.min;
                } else {
                    range.max = range.min;
                }
                ImGui::PopID();
            };
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Preview", nullptr, true)) {
                if (ImGui::SmallButton(ps.playing && !ps.paused ? "Pause" : "Play")) {
                    if (ps.playing && !ps.paused) {
                        ps.paused = true;
                    } else {
                        ps.playing = true;
                        ps.paused = false;
                    }
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Restart")) {
                    ParticleRuntime::Restart(ps);
                    ps.playing = true;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Stop")) {
                    ParticleRuntime::Restart(ps);
                    ps.playing = false;
                    changed = true;
                }
                ImGui::TextDisabled("Live Particles: %d", static_cast<int>(ps.particles.size()));
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Main", nullptr, true)) {
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_LOOPING", "Looping"), &ps.looping);
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_PREWARM", "Prewarm"), &ps.prewarm);
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_PLAY_ON_AWAKE", "Play On Awake"), &ps.playOnAwake);
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_START_DELAY", "Start Delay"), &ps.startDelay, 0.01f, 0.0f, 1000.0f, "%.2f");
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_GRAVITY", "Gravity"), &ps.gravityModifier, 0.01f, -100.0f, 100.0f, "%.2f");
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_SIMULATION_SPEED", "Simulation Speed"), &ps.simulationSpeed, 0.01f, 0.0f, 20.0f, "%.2f");
                const char* spaces[] = { "Local", "World" };
                if (ImGui::Combo("Simulation Space", &ps.simulationSpace, spaces, IM_ARRAYSIZE(spaces))) {
                    // Live particles are stored in the old space; start clean rather than
                    // reinterpret every one of them.
                    ParticleRuntime::Restart(ps);
                    ps.playing = true;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Local: particles move with the emitter.\nWorld: they stay where they were emitted (trails, smoke).");
                }
                changed |= ImGui::DragInt("Max Particles", &ps.maxParticles, 1.0f, 1, 100000);
                changed |= ImGui::ColorEdit4(Loc::Widget("COMPONENT_AUDIO_MODE_START_COLOR", "Start Color"), &ps.startColor.x);
                if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Start Lifetime", nullptr, true)) minMaxRow("StartLifetime", ps.startLifetime);
                if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Start Speed", nullptr, true)) minMaxRow("StartSpeed", ps.startSpeed);
                if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Start Size", nullptr, true)) minMaxRow("StartSize", ps.startSize);
                if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Start Rotation", nullptr, true)) minMaxRow("StartRotation", ps.startRotation);
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Emission", nullptr, true)) {
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_RATE", "Rate"), &ps.emissionRate, 0.1f, 0.0f, 10000.0f, "%.1f");
                changed |= ImGui::DragInt("Burst Count", &ps.burstCount, 1.0f, 0, 100000);
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_BURST_TIME", "Burst Time"), &ps.burstTime, 0.01f, 0.0f, 1000.0f, "%.2f");
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_BURST_LOOP", "Burst Loop"), &ps.burstLoop);
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Shape", nullptr, true)) {
                const char* shapes[] = { "Point", "Circle / Sphere", "Box", "Cone", "Hemisphere", "Edge" };
                changed |= ImGui::Combo(Loc::Widget("COMPONENT_AUDIO_MODE_EMITTER_SHAPE", "Emitter Shape"), &ps.shape, shapes, IM_ARRAYSIZE(shapes));
                changed |= ImGui::Checkbox("Emit In 3D", &ps.emitIn3D);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Spread particles through depth too: a sphere instead of a circle,\na disc-shaped cone instead of a fan. Off keeps everything on the emitter's XY plane.");
                }
                if (ps.shape != ParticleSystem2DComponent::ShapePoint &&
                    ps.shape != ParticleSystem2DComponent::ShapeBox) {
                    changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_RADIUS", "Radius"), &ps.shapeRadius, 0.01f, 0.0f, 10000.0f, "%.2f");
                }
                if (ps.shape == ParticleSystem2DComponent::ShapeCone) {
                    changed |= ImGui::SliderFloat("Angle", &ps.shapeAngle, 0.0f, 89.0f, "%.1f deg");
                }
                if (ps.shape == ParticleSystem2DComponent::ShapeBox) {
                    if (ps.emitIn3D) {
                        changed |= ImGui::DragFloat3("Box", &ps.shapeBox.x, 0.01f, 0.0f, 10000.0f, "%.2f");
                    } else {
                        changed |= ImGui::DragFloat2("Box", &ps.shapeBox.x, 0.01f, 0.0f, 10000.0f, "%.2f");
                    }
                }
                if (ps.shape == ParticleSystem2DComponent::ShapeCone ||
                    ps.shape == ParticleSystem2DComponent::ShapeHemisphere ||
                    ps.shape == ParticleSystem2DComponent::ShapeEdge) {
                    ImGui::TextDisabled("Emits along the emitter's +Y; rotate the object to aim it.");
                }
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Velocity over Lifetime")) {
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_ENABLED_VEL_LIFE", "Enabled##VelLife"), &ps.velocityOverLifetimeEnabled);
                changed |= ImGui::DragFloat3("Velocity", &ps.velocityOverLifetime.x, 0.01f, -1000.0f, 1000.0f, "%.2f");
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Color over Lifetime")) {
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_ENABLED_COLOR_LIFE", "Enabled##ColorLife"), &ps.colorOverLifetimeEnabled);
                changed |= ImGui::ColorEdit4(Loc::Widget("COMPONENT_AUDIO_MODE_END_COLOR", "End Color"), &ps.colorOverLifetime.x);
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Size over Lifetime")) {
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_ENABLED_SIZE_LIFE", "Enabled##SizeLife"), &ps.sizeOverLifetimeEnabled);
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_END_SIZE", "End Size"), &ps.sizeOverLifetime, 0.01f, 0.0f, 1000.0f, "%.2f");
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Rotation over Lifetime")) {
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_ENABLED_ROT_LIFE", "Enabled##RotLife"), &ps.rotationOverLifetimeEnabled);
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_ANGULAR_VELOCITY", "Angular Velocity"), &ps.rotationOverLifetime, 0.1f, -10000.0f, 10000.0f, "%.1f");
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Noise")) {
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_ENABLED_NOISE", "Enabled##Noise"), &ps.noiseEnabled);
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_STRENGTH", "Strength"), &ps.noiseStrength, 0.01f, 0.0f, 1000.0f, "%.2f");
                changed |= ImGui::DragFloat(Loc::Widget("COMPONENT_AUDIO_MODE_FREQUENCY", "Frequency"), &ps.noiseFrequency, 0.01f, 0.01f, 1000.0f, "%.2f");
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Texture Sheet Animation")) {
                changed |= ImGui::Checkbox("Enabled##TextureSheet", &ps.textureSheetEnabled);
                const char* sheetModes[] = { "Grid", "Sprite Clips" };
                changed |= ImGui::Combo("Mode##TextureSheet", &ps.textureSheetMode, sheetModes, IM_ARRAYSIZE(sheetModes));
                if (ps.textureSheetMode == ParticleSystem2DComponent::SheetGrid) {
                    int tiles[2] = { ps.textureSheetTilesX, ps.textureSheetTilesY };
                    if (ImGui::DragInt2("Tiles (X, Y)", tiles, 0.1f, 1, 256)) {
                        ps.textureSheetTilesX = std::clamp(tiles[0], 1, 256);
                        ps.textureSheetTilesY = std::clamp(tiles[1], 1, 256);
                        changed = true;
                    }
                } else if (!ps.textureSheetEnabled) {
                    ImGui::TextDisabled("Uses the clips of the texture's .spritesheet.");
                } else if (!ps.runtimeSheetRects.empty()) {
                    ImGui::TextDisabled("%d clips from %s.spritesheet",
                                        static_cast<int>(ps.runtimeSheetRects.size()),
                                        fs::path(ParticleRuntime::TexturePath(obj)).filename().string().c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                                       "No .spritesheet next to the texture - slice it in the\nPixel Sprite Editor, or use Grid.");
                }
                const char* timings[] = { "Over Lifetime", "Frames Per Second" };
                changed |= ImGui::Combo("Timing", &ps.textureSheetTiming, timings, IM_ARRAYSIZE(timings));
                if (ps.textureSheetTiming == ParticleSystem2DComponent::SheetFps) {
                    changed |= ImGui::DragFloat("FPS", &ps.textureSheetFps, 0.1f, 0.0f, 240.0f, "%.1f");
                    changed |= ImGui::Checkbox("Loop##TextureSheet", &ps.textureSheetLoop);
                } else {
                    changed |= ImGui::DragFloat("Cycles", &ps.textureSheetCycles, 0.05f, 0.0f, 100.0f, "%.2f");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("How many times the animation plays over one particle's life.");
                    }
                }
                changed |= ImGui::DragInt("Start Frame", &ps.textureSheetStartFrame, 0.1f, 0, 4096);
                changed |= ImGui::DragInt("Frame Count", &ps.textureSheetFrameCount, 0.1f, 0, 4096, ps.textureSheetFrameCount == 0 ? "All" : "%d");
                changed |= ImGui::Checkbox("Random Start Frame", &ps.textureSheetRandomStart);
            }
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Renderer", nullptr, true)) {
                changed |= drawAssetReferenceSlot("##ParticleTexture", "Texture", ps.texturePath,
                                                  FileCategory::Texture, "Texture");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Overrides the material's texture. Empty: the material's, else the object's.");
                }
                changed |= drawAssetReferenceSlot("##ParticleMaterial", "Material", ps.materialPath,
                                                  FileCategory::Material, "Material");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Tints particles with its colour and opacity, adds its emission,\nand supplies a texture when Texture is empty.");
                }
                const char* alignments[] = { "Billboard", "Stretched", "Horizontal", "Vertical", "Local" };
                changed |= ImGui::Combo("Render Alignment", &ps.renderAlignment, alignments, IM_ARRAYSIZE(alignments));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("How quads face the camera in 3D views.\nBillboard: always face it. Stretched: streak along their motion.\nHorizontal: lie flat. Vertical: stand upright. Local: the emitter's XY plane.");
                }
                if (ps.renderAlignment == ParticleSystem2DComponent::AlignStretched) {
                    changed |= ImGui::DragFloat("Length Scale", &ps.stretchLengthScale, 0.01f, 0.0f, 100.0f, "%.2f");
                    changed |= ImGui::DragFloat("Speed Scale", &ps.stretchSpeedScale, 0.005f, 0.0f, 10.0f, "%.3f");
                }
                const char* blendModes[] = { "Alpha", "Additive", "Premultiplied", "Multiply" };
                changed |= ImGui::Combo("Blend Mode", &ps.blendMode, blendModes, IM_ARRAYSIZE(blendModes));
                ImGui::TextDisabled("2D lighting (2D views)");
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_RECEIVE_LIGHTING", "Receive Lighting"), &ps.receiveLighting2D);
                changed |= ImGui::Checkbox(Loc::Widget("COMPONENT_AUDIO_MODE_FORCE_UNLIT", "Force Unlit"), &ps.unlitLighting2D);
                changed |= ImGui::SliderFloat(Loc::Widget("COMPONENT_AUDIO_MODE_EMISSIVE", "Emissive"), &ps.emissiveLighting2D, 0.0f, 8.0f, "%.2f");
            }
        }
        if (changed) {
            particleSystem2DSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
    }

    if (inspectorComponentKey == "ground_baked" && obj.hasGroundBakedType && sharedGroundBaked) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.28f, 0.5f, 0.34f, 1.0f));
        bool removeGroundBaked = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_GROUND_BAKED_TYPE", "GroundBakedType"), "GroundBakedType", "ground_baked", &obj.groundBakedType.enabled, true, [&]() {
            drawStandardComponentMenu(
                "ground_baked",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::GroundBaked,
                [&]() { obj.groundBakedType = GroundBakedTypeComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::GroundBaked; inspectorClipboard.groundBaked = obj.groundBakedType; },
                [&]() { obj.hasGroundBakedType = true; obj.groundBakedType = inspectorClipboard.groundBaked; changed = true; },
                [&]() { obj.groundBakedType = inspectorClipboard.groundBaked; changed = true; },
                removeGroundBaked);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("GroundBakedType");
            if (ImGui::Button("Open AI Pathfinding")) {
                showAIPathfindingWindow = true;
            }
            if (beginCompFields("##Fields_GroundBaked")) {
                if (boolRow(Loc::T("COMPONENT_GROUND_BAKED_TYPE_INCLUDE_IN_BAKE", "Include In Bake"), &obj.groundBakedType.includeInBake)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_GROUND_BAKED_TYPE_AREA_COST", "Area Cost"));
                if (ImGui::DragFloat("##AreaCost", &obj.groundBakedType.areaCost, 0.05f, 0.1f, 100.0f, "%.2f")) {
                    obj.groundBakedType.areaCost = std::clamp(obj.groundBakedType.areaCost, 0.1f, 100.0f);
                    changed = true;
                }
                noteRow("Objects marked here are considered walkable during bake.");
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeGroundBaked) {
            obj.hasGroundBakedType = false;
            obj.groundBakedType = GroundBakedTypeComponent{};
            changed = true;
        }
        if (changed) {
            groundBakedSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    auto mapSectorCombo = [&](const char* rowLabel, const char* comboId, std::string& sectorIdRef) -> bool {
        bool changedCombo = false;
        const SceneObject* current = MapMaker::FindSectorById(sceneObjects, sectorIdRef);
        const char* preview = current != nullptr ? current->name.c_str()
                              : (sectorIdRef.empty() ? "None" : "(missing sector)");
        fieldRow(rowLabel);
        if (ImGui::BeginCombo(comboId, preview)) {
            if (ImGui::Selectable("None", sectorIdRef.empty())) {
                if (!sectorIdRef.empty()) { sectorIdRef.clear(); changedCombo = true; }
            }
            for (const SceneObject& candidate : sceneObjects) {
                if (!candidate.hasMapSector) continue;
                ImGui::PushID(candidate.id);
                const bool selectedEntry = candidate.mapSector.sectorId == sectorIdRef;
                if (ImGui::Selectable(candidate.name.c_str(), selectedEntry) &&
                    candidate.mapSector.sectorId != sectorIdRef) {
                    sectorIdRef = candidate.mapSector.sectorId;
                    changedCombo = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", candidate.mapSector.sectorId.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return changedCombo;
    };
    auto mapFocusObject = [&](int objectId) {
        if (objectId < 0) return;
        setPrimarySelection(objectId, false);
        focusViewportOnSelection();
    };
    auto mapStringRow = [&](const char* rowLabel, const char* inputId, std::string& value,
                            size_t capacity = 256) -> bool {
        char buffer[512];
        if (capacity > sizeof(buffer)) capacity = sizeof(buffer);
        std::snprintf(buffer, capacity, "%s", value.c_str());
        fieldRow(rowLabel);
        if (ImGui::InputText(inputId, buffer, capacity)) {
            value = buffer;
            return true;
        }
        return false;
    };
    auto mapReadOnlyIdRow = [&](const char* rowLabel, const std::string& id) {
        fieldRow(rowLabel);
        ImGui::TextDisabled("%s", id.empty() ? "(none)" : id.c_str());
        if (!id.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stable id used by cross references. Right-click header for regeneration.");
        }
    };

    if (inspectorComponentKey == "map_root" && obj.hasMapRoot && sharedMapRoot) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.48f, 0.44f, 1.0f));
        bool removeMapRoot = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_MAP_ROOT", "Map Root"), "MapRoot", "map_root", &obj.mapRoot.enabled, true, [&]() {
            drawReorderMenuItems("map_root");
            ImGui::Separator();
            if (ImGui::MenuItem("Regenerate Map Id")) {
                obj.mapRoot.mapId = MapMaker::GenerateId("map");
                changed = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeMapRoot = true;
            }
        });
        if (header.enabledChanged) changed = true;
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("MapRoot");
            int sectorCount = 0;
            int transitionCount = 0;
            for (const SceneObject& candidate : sceneObjects) {
                if (candidate.hasMapSector) ++sectorCount;
                if (candidate.hasMapTransition) ++transitionCount;
            }
            if (ImGui::Button("Open Sector Map")) {
                showSectorMapWindow = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Validate Map")) {
                const auto issues = MapMaker::ValidateMap(sceneObjects);
                if (issues.empty()) {
                    addConsoleMessage("Map validation: no problems found", ConsoleMessageType::Success);
                } else {
                    for (const auto& issue : issues) {
                        const ConsoleMessageType type =
                            issue.severity == MapMaker::MapIssueSeverity::Error ? ConsoleMessageType::Error
                            : issue.severity == MapMaker::MapIssueSeverity::Warning ? ConsoleMessageType::Warning
                                                                                    : ConsoleMessageType::Info;
                        addConsoleMessage("Map validation: " + issue.message, type);
                    }
                    addConsoleMessage("Map validation finished: " + std::to_string(issues.size()) + " issue(s)",
                                      ConsoleMessageType::Info);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Rebuild References")) {
                int fixedCount = 0;
                for (SceneObject& portalObj : sceneObjects) {
                    if (!portalObj.hasMapPortal) continue;
                    const SceneObject* owner = MapMaker::FindOwningSector(sceneObjects, portalObj);
                    if (owner != nullptr && portalObj.mapPortal.sectorId != owner->mapSector.sectorId) {
                        portalObj.mapPortal.sectorId = owner->mapSector.sectorId;
                        ++fixedCount;
                    }
                }
                for (SceneObject& transitionObj : sceneObjects) {
                    if (!transitionObj.hasMapTransition) continue;
                    MapTransitionComponent& t = transitionObj.mapTransition;
                    if (!t.sourcePortalId.empty() &&
                        MapMaker::FindPortalById(sceneObjects, t.sourcePortalId) == nullptr) {
                        t.sourcePortalId.clear();
                        ++fixedCount;
                    }
                    if (!t.destinationPortalId.empty() &&
                        MapMaker::FindPortalById(sceneObjects, t.destinationPortalId) == nullptr) {
                        t.destinationPortalId.clear();
                        ++fixedCount;
                    }
                }
                if (obj.mapRoot.startSectorId.empty() ||
                    MapMaker::FindSectorById(sceneObjects, obj.mapRoot.startSectorId) == nullptr) {
                    for (const SceneObject& candidate : sceneObjects) {
                        if (candidate.hasMapSector) {
                            obj.mapRoot.startSectorId = candidate.mapSector.sectorId;
                            ++fixedCount;
                            break;
                        }
                    }
                }
                addConsoleMessage("Map references rebuilt (" + std::to_string(fixedCount) + " change(s))",
                                  fixedCount > 0 ? ConsoleMessageType::Success : ConsoleMessageType::Info);
                if (fixedCount > 0) changed = true;
            }
            if (beginCompFields("##Fields_MapRoot")) {
                mapReadOnlyIdRow("Map Id", obj.mapRoot.mapId);
                fieldRow(Loc::T("COMPONENT_MAP_ROOT_SECTORS", "Sectors"));
                ImGui::TextDisabled("%d", sectorCount);
                fieldRow(Loc::T("COMPONENT_MAP_ROOT_TRANSITIONS", "Transitions"));
                ImGui::TextDisabled("%d", transitionCount);
                if (mapSectorCombo("Start Sector", "##MapRootStartSector", obj.mapRoot.startSectorId)) changed = true;
                fieldRow(Loc::T("COMPONENT_MAP_ROOT_SECTOR_VISIBILITY", "Sector Visibility"));
                static const char* kVisibilityModes[] = { "Show All Sectors", "Current + Adjacent", "Current Only" };
                int visibilityMode = std::clamp(obj.mapRoot.sectorVisibilityMode, 0, 2);
                if (ImGui::Combo("##MapRootVisibility", &visibilityMode, kVisibilityModes, 3)) {
                    obj.mapRoot.sectorVisibilityMode = visibilityMode;
                    applyMapSectorVisibility();
                    changed = true;
                }
                if (mapStringRow("Notes", "##MapRootNotes", obj.mapRoot.notes)) changed = true;
                noteRow("Sector visibility only affects the editor viewport. Runtime streaming reads the sector metadata instead.");
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeMapRoot) {
            obj.hasMapRoot = false;
            obj.mapRoot = MapRootComponent{};
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "map_sector" && obj.hasMapSector && sharedMapSector) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f, 0.42f, 0.58f, 1.0f));
        bool removeMapSector = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_MAP_SECTOR", "Sector"), "MapSector", "map_sector", &obj.mapSector.enabled, true, [&]() {
            drawReorderMenuItems("map_sector");
            ImGui::Separator();
            if (ImGui::MenuItem("Regenerate Sector Id")) {
                const std::string oldId = obj.mapSector.sectorId;
                obj.mapSector.sectorId = MapMaker::GenerateId("sec");
                for (SceneObject& other : sceneObjects) {
                    if (other.hasMapTransition) {
                        if (other.mapTransition.sourceSectorId == oldId) other.mapTransition.sourceSectorId = obj.mapSector.sectorId;
                        if (other.mapTransition.destinationSectorId == oldId) other.mapTransition.destinationSectorId = obj.mapSector.sectorId;
                    }
                    if (other.hasMapPortal && other.mapPortal.sectorId == oldId) {
                        other.mapPortal.sectorId = obj.mapSector.sectorId;
                    }
                    if (other.hasMapRoot) {
                        if (other.mapRoot.startSectorId == oldId) other.mapRoot.startSectorId = obj.mapSector.sectorId;
                        if (other.mapRoot.activeSectorId == oldId) other.mapRoot.activeSectorId = obj.mapSector.sectorId;
                    }
                }
                changed = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeMapSector = true;
            }
        });
        if (header.enabledChanged) changed = true;
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("MapSector");
            if (ImGui::Button("Focus Sector")) {
                mapFocusObject(obj.id);
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Sector Map")) {
                showSectorMapWindow = true;
            }
            if (beginCompFields("##Fields_MapSector")) {
                mapReadOnlyIdRow("Sector Id", obj.mapSector.sectorId);
                fieldRow(Loc::T("COMPONENT_MAP_SECTOR_EDITOR_COLOR", "Editor Color"));
                if (ImGui::ColorEdit3("##MapSectorColor", &obj.mapSector.color.x,
                                      ImGuiColorEditFlags_NoInputs)) {
                    changed = true;
                }
                if (boolRow(Loc::T("COMPONENT_MAP_SECTOR_CUSTOM_BOUNDS", "Custom Bounds"), &obj.mapSector.useCustomBounds)) changed = true;
                if (obj.mapSector.useCustomBounds) {
                    fieldRow(Loc::T("COMPONENT_MAP_SECTOR_BOUNDS_CENTER", "Bounds Center"));
                    if (ImGui::DragFloat3("##MapSectorBoundsCenter", &obj.mapSector.boundsCenter.x, 0.1f)) changed = true;
                    fieldRow(Loc::T("COMPONENT_MAP_SECTOR_BOUNDS_SIZE", "Bounds Size"));
                    if (ImGui::DragFloat3("##MapSectorBoundsSize", &obj.mapSector.boundsSize.x, 0.1f, 0.0f, 4096.0f)) changed = true;
                }
                if (mapStringRow("Streaming Tag", "##MapSectorStreamingTag", obj.mapSector.streamingTag)) changed = true;
                if (mapStringRow("Notes", "##MapSectorNotes", obj.mapSector.notes)) changed = true;
                fieldRow(Loc::T("COMPONENT_MAP_SECTOR_EST_MEMORY", "Est. Memory"));
                ImGui::TextDisabled("%s", obj.mapSector.estimatedMemoryMB > 0.0f
                                              ? (std::to_string(obj.mapSector.estimatedMemoryMB) + " MB").c_str()
                                              : "(not estimated)");
                int connectionCount = 0;
                for (const SceneObject& candidate : sceneObjects) {
                    if (candidate.hasMapTransition &&
                        MapMaker::TransitionTouchesSector(candidate.mapTransition, obj.mapSector.sectorId)) {
                        ++connectionCount;
                    }
                }
                fieldRow(Loc::T("COMPONENT_MAP_SECTOR_CONNECTIONS", "Connections"));
                ImGui::TextDisabled("%d", connectionCount);
                endCompFields();
            }
            for (const SceneObject& candidate : sceneObjects) {
                if (!candidate.hasMapTransition ||
                    !MapMaker::TransitionTouchesSector(candidate.mapTransition, obj.mapSector.sectorId)) {
                    continue;
                }
                ImGui::PushID(candidate.id);
                const SceneObject* otherSector = MapMaker::FindSectorById(
                    sceneObjects,
                    candidate.mapTransition.sourceSectorId == obj.mapSector.sectorId
                        ? candidate.mapTransition.destinationSectorId
                        : candidate.mapTransition.sourceSectorId);
                std::string label = std::string(MapMaker::TransitionKindLabel(candidate.mapTransition.kind)) +
                                    " -> " + (otherSector != nullptr ? otherSector->name : "(missing)");
                if (ImGui::SmallButton("Select")) {
                    setPrimarySelection(candidate.id, false);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", label.c_str());
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        if (removeMapSector) {
            obj.hasMapSector = false;
            obj.mapSector = MapSectorComponent{};
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "map_transition" && obj.hasMapTransition && sharedMapTransition) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.52f, 0.42f, 0.24f, 1.0f));
        bool removeMapTransition = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_MAP_TRANSITION", "Transition"), "MapTransition", "map_transition", &obj.mapTransition.enabled, true, [&]() {
            drawReorderMenuItems("map_transition");
            ImGui::Separator();
            if (ImGui::MenuItem("Regenerate Transition Id")) {
                const std::string oldId = obj.mapTransition.transitionId;
                obj.mapTransition.transitionId = MapMaker::GenerateId("trn");
                for (SceneObject& other : sceneObjects) {
                    if (other.hasMapPortal && other.mapPortal.transitionId == oldId) {
                        other.mapPortal.transitionId = obj.mapTransition.transitionId;
                    }
                }
                changed = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeMapTransition = true;
            }
        });
        if (header.enabledChanged) changed = true;
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("MapTransition");
            {
                const SceneObject* src = MapMaker::FindSectorById(sceneObjects, obj.mapTransition.sourceSectorId);
                const SceneObject* dst = MapMaker::FindSectorById(sceneObjects, obj.mapTransition.destinationSectorId);
                ImGui::BeginDisabled(src == nullptr);
                if (ImGui::Button("Focus Source")) {
                    mapFocusObject(src != nullptr ? src->id : -1);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(dst == nullptr);
                if (ImGui::Button("Focus Destination")) {
                    mapFocusObject(dst != nullptr ? dst->id : -1);
                }
                ImGui::EndDisabled();
            }
            if (beginCompFields("##Fields_MapTransition")) {
                mapReadOnlyIdRow("Transition Id", obj.mapTransition.transitionId);
                if (mapSectorCombo("Source Sector", "##MapTransitionSource", obj.mapTransition.sourceSectorId)) changed = true;
                if (mapSectorCombo("Destination", "##MapTransitionDest", obj.mapTransition.destinationSectorId)) changed = true;
                fieldRow(Loc::T("COMPONENT_MAP_TRANSITION_TYPE", "Type"));
                {
                    int kindIndex = std::clamp(static_cast<int>(obj.mapTransition.kind), 0,
                                               MapMaker::kMapTransitionKindCount - 1);
                    if (ImGui::BeginCombo("##MapTransitionKind",
                                          MapMaker::TransitionKindLabel(static_cast<MapTransitionKind>(kindIndex)))) {
                        for (int i = 0; i < MapMaker::kMapTransitionKindCount; ++i) {
                            if (ImGui::Selectable(MapMaker::TransitionKindLabel(static_cast<MapTransitionKind>(i)),
                                                  i == kindIndex)) {
                                obj.mapTransition.kind = static_cast<MapTransitionKind>(i);
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                if (boolRow(Loc::T("COMPONENT_MAP_TRANSITION_BIDIRECTIONAL", "Bidirectional"), &obj.mapTransition.bidirectional)) changed = true;
                if (boolRow(Loc::T("COMPONENT_MAP_TRANSITION_LOCKED", "Locked"), &obj.mapTransition.locked)) changed = true;
                if (mapStringRow("Condition", "##MapTransitionCondition", obj.mapTransition.condition)) changed = true;
                if (mapStringRow("Editor Label", "##MapTransitionLabel", obj.mapTransition.editorLabel)) changed = true;
                {
                    const SceneObject* srcPortal = MapMaker::FindPortalById(sceneObjects, obj.mapTransition.sourcePortalId);
                    fieldRow(Loc::T("COMPONENT_MAP_TRANSITION_SOURCE_PORTAL", "Source Portal"));
                    if (srcPortal != nullptr) {
                        if (ImGui::SmallButton("Select##SrcPortal")) setPrimarySelection(srcPortal->id, false);
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", srcPortal->name.c_str());
                    } else {
                        ImGui::TextDisabled("%s", obj.mapTransition.sourcePortalId.empty() ? "(none)" : "(missing)");
                    }
                    const SceneObject* dstPortal = MapMaker::FindPortalById(sceneObjects, obj.mapTransition.destinationPortalId);
                    fieldRow(Loc::T("COMPONENT_MAP_TRANSITION_DEST_PORTAL", "Dest Portal"));
                    if (dstPortal != nullptr) {
                        if (ImGui::SmallButton("Select##DstPortal")) setPrimarySelection(dstPortal->id, false);
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", dstPortal->name.c_str());
                    } else {
                        ImGui::TextDisabled("%s", obj.mapTransition.destinationPortalId.empty() ? "(none)" : "(missing)");
                    }
                }
                if (boolRow(Loc::T("COMPONENT_MAP_TRANSITION_ENTRY_TRANSFORM", "Entry Transform"), &obj.mapTransition.hasEntryTransform)) changed = true;
                if (obj.mapTransition.hasEntryTransform) {
                    fieldRow(Loc::T("COMPONENT_MAP_TRANSITION_ENTRY_POSITION", "Entry Position"));
                    if (ImGui::DragFloat3("##MapTransitionEntryPos", &obj.mapTransition.entryPosition.x, 0.1f)) changed = true;
                    fieldRow(Loc::T("COMPONENT_MAP_TRANSITION_ENTRY_YAW", "Entry Yaw"));
                    if (ImGui::DragFloat("##MapTransitionEntryYaw", &obj.mapTransition.entryYawDeg, 1.0f, -360.0f, 360.0f, "%.0f deg")) changed = true;
                }
                noteRow("Condition is stored as plain data (key/flag/tag). Gameplay scripts decide what it means.");
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeMapTransition) {
            obj.hasMapTransition = false;
            obj.mapTransition = MapTransitionComponent{};
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "map_portal" && obj.hasMapPortal && sharedMapPortal) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.42f, 0.3f, 0.52f, 1.0f));
        bool removeMapPortal = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_MAP_PORTAL", "Portal"), "MapPortal", "map_portal", &obj.mapPortal.enabled, true, [&]() {
            drawReorderMenuItems("map_portal");
            ImGui::Separator();
            if (ImGui::MenuItem("Regenerate Portal Id")) {
                const std::string oldId = obj.mapPortal.portalId;
                obj.mapPortal.portalId = MapMaker::GenerateId("prt");
                for (SceneObject& other : sceneObjects) {
                    if (!other.hasMapTransition) continue;
                    if (other.mapTransition.sourcePortalId == oldId) other.mapTransition.sourcePortalId = obj.mapPortal.portalId;
                    if (other.mapTransition.destinationPortalId == oldId) other.mapTransition.destinationPortalId = obj.mapPortal.portalId;
                }
                changed = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeMapPortal = true;
            }
        });
        if (header.enabledChanged) changed = true;
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("MapPortal");
            {
                SceneObject* transitionObj = MapMaker::FindTransitionById(sceneObjects, obj.mapPortal.transitionId);
                ImGui::BeginDisabled(transitionObj == nullptr);
                if (ImGui::Button("Select Transition")) {
                    setPrimarySelection(transitionObj != nullptr ? transitionObj->id : -1, false);
                }
                ImGui::EndDisabled();
                const SceneObject* sectorObj = MapMaker::FindSectorById(sceneObjects, obj.mapPortal.sectorId);
                ImGui::SameLine();
                ImGui::BeginDisabled(sectorObj == nullptr);
                if (ImGui::Button("Focus Sector")) {
                    mapFocusObject(sectorObj != nullptr ? sectorObj->id : -1);
                }
                ImGui::EndDisabled();
            }
            if (beginCompFields("##Fields_MapPortal")) {
                mapReadOnlyIdRow("Portal Id", obj.mapPortal.portalId);
                mapReadOnlyIdRow("Transition", obj.mapPortal.transitionId);
                if (mapSectorCombo("Owning Sector", "##MapPortalSector", obj.mapPortal.sectorId)) changed = true;
                fieldRow(Loc::T("COMPONENT_MAP_PORTAL_OPENING_SIZE", "Opening Size"));
                if (ImGui::DragFloat2("##MapPortalOpening", &obj.mapPortal.openingSize.x, 0.05f, 0.1f, 64.0f, "%.2f")) changed = true;
                noteRow("Portals mark doorway openings. Transitions reference them to know where rooms connect.");
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeMapPortal) {
            obj.hasMapPortal = false;
            obj.mapPortal = MapPortalComponent{};
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "map_mesh" && obj.hasMapMesh && sharedMapMesh) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.28f, 0.48f, 0.3f, 1.0f));
        bool removeMapMesh = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_MAP_MESH", "Map Mesh"), "MapMesh", "map_mesh", &obj.mapMesh.enabled, true, [&]() {
            drawStandardComponentMenu(
                "map_mesh",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::MapMesh,
                [&]() { obj.mapMesh = MapMeshComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::MapMesh; inspectorClipboard.mapMesh = obj.mapMesh; },
                [&]() { obj.hasMapMesh = true; obj.mapMesh = inspectorClipboard.mapMesh; changed = true; },
                [&]() { obj.mapMesh = inspectorClipboard.mapMesh; changed = true; },
                removeMapMesh);
        });
        if (header.enabledChanged) changed = true;
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("MapMesh");
            const bool meshEditable = IsMeshEditablePath(obj.meshPath);
            if (!meshEditable) {
                ImGui::TextDisabled("Mesh is not editable (needs a .rmesh or .mmesh asset).");
            } else {
                ImGui::TextDisabled("Select the object in the viewport to use Object / Mesh Edit / Carve modes.");
            }
            ImGui::BeginDisabled(!meshEditable);
            if (ImGui::Button("Validate Mesh")) {
                RawMeshAsset meshCopy;
                std::string loadError;
                if (getModelLoader().loadRawMesh(obj.meshPath, meshCopy, loadError)) {
                    const auto problems = MapMaker::ValidateMeshAsset(meshCopy);
                    if (problems.empty()) {
                        addConsoleMessage("Mesh validation: no problems found in " + obj.meshPath,
                                          ConsoleMessageType::Success);
                    } else {
                        for (const std::string& problem : problems) {
                            addConsoleMessage("Mesh validation: " + problem, ConsoleMessageType::Warning);
                        }
                    }
                } else {
                    addConsoleMessage("Mesh validation load failed: " + loadError, ConsoleMessageType::Error);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Recalculate Normals")) {
                if (ensureMeshEditTarget(&obj)) {
                    recordState("mapMeshRecalculateNormals");
                    meshEditAsset.normals.assign(meshEditAsset.positions.size(), glm::vec3(0.0f));
                    for (const auto& face : meshEditAsset.faces) {
                        if (face.x >= meshEditAsset.positions.size() ||
                            face.y >= meshEditAsset.positions.size() ||
                            face.z >= meshEditAsset.positions.size()) continue;
                        const glm::vec3 n = glm::cross(
                            meshEditAsset.positions[face.y] - meshEditAsset.positions[face.x],
                            meshEditAsset.positions[face.z] - meshEditAsset.positions[face.x]);
                        meshEditAsset.normals[face.x] += n;
                        meshEditAsset.normals[face.y] += n;
                        meshEditAsset.normals[face.z] += n;
                    }
                    for (auto& n : meshEditAsset.normals) {
                        if (glm::length(n) > 1e-6f) n = glm::normalize(n);
                    }
                    meshEditAsset.hasNormals = true;
                    meshEditDirty = true;
                    syncMeshEditToGPU(&obj);
                    addConsoleMessage("Recalculated normals for " + obj.meshPath, ConsoleMessageType::Success);
                } else {
                    addConsoleMessage("Recalculate normals: mesh could not be loaded for editing",
                                      ConsoleMessageType::Error);
                }
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Generate Collision")) {
                obj.hasCollider = true;
                obj.collider.enabled = true;
                obj.collider.type = ColliderType::Mesh;
                obj.collider.convex = false;
                changed = true;
                addConsoleMessage("Added static mesh collider to " + obj.name, ConsoleMessageType::Success);
            }
            if (beginCompFields("##Fields_MapMesh")) {
                fieldRow(Loc::T("COMPONENT_MAP_MESH_GRID_SIZE", "Grid Size"));
                if (ImGui::DragFloat("##MapMeshGridSize", &obj.mapMesh.gridSize, 0.05f, 0.01f, 16.0f, "%.2f")) {
                    obj.mapMesh.gridSize = std::clamp(obj.mapMesh.gridSize, 0.01f, 16.0f);
                    changed = true;
                }
                if (boolRow(Loc::T("COMPONENT_MAP_MESH_SNAP_TO_GRID", "Snap To Grid"), &obj.mapMesh.snapToGrid)) changed = true;
                if (boolRow(Loc::T("COMPONENT_MAP_MESH_VERTEX_SNAPPING", "Vertex Snapping"), &obj.mapMesh.vertexSnapping)) changed = true;
                if (boolRow(Loc::T("COMPONENT_MAP_MESH_SURFACE_SNAPPING", "Surface Snapping"), &obj.mapMesh.surfaceSnapping)) changed = true;
                if (boolRow(Loc::T("COMPONENT_MAP_MESH_AUTO_COLLISION", "Auto Collision"), &obj.mapMesh.autoCollision)) changed = true;
                noteRow("Marks this object as map geometry. Grid settings drive Mesh Edit and Carve snapping.");
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeMapMesh) {
            obj.hasMapMesh = false;
            obj.mapMesh = MapMeshComponent{};
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "obstacle" && obj.hasObsticleObject && sharedObstacle) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.5f, 0.3f, 0.28f, 1.0f));
        bool removeObstacle = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_OBSTICLE_OBJECT", "ObsticleObject"), "ObsticleObject", "obstacle", &obj.obsticleObject.enabled, true, [&]() {
            drawStandardComponentMenu(
                "obstacle",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Obstacle,
                [&]() { obj.obsticleObject = ObsticleObjectComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::Obstacle; inspectorClipboard.obstacle = obj.obsticleObject; },
                [&]() { obj.hasObsticleObject = true; obj.obsticleObject = inspectorClipboard.obstacle; changed = true; },
                [&]() { obj.obsticleObject = inspectorClipboard.obstacle; changed = true; },
                removeObstacle);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("ObsticleObject");
            if (ImGui::Button("Open AI Pathfinding")) {
                showAIPathfindingWindow = true;
            }
            if (beginCompFields("##Fields_Obstacle")) {
                if (boolRow(Loc::T("COMPONENT_OBSTICLE_OBJECT_CARVE", "Carve"), &obj.obsticleObject.carve)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_OBSTICLE_OBJECT_PADDING", "Padding"));
                if (ImGui::DragFloat("##Padding", &obj.obsticleObject.padding, 0.02f, 0.0f, 10.0f, "%.2f")) {
                    obj.obsticleObject.padding = std::clamp(obj.obsticleObject.padding, 0.0f, 10.0f);
                    changed = true;
                }
                noteRow("Obstacle regions are removed from the baked walkable map.");
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeObstacle) {
            obj.hasObsticleObject = false;
            obj.obsticleObject = ObsticleObjectComponent{};
            changed = true;
        }
        if (changed) {
            obstacleSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "ai_agent" && obj.hasAIAgent && sharedAgent) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.32f, 0.4f, 0.58f, 1.0f));
        bool removeAgent = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_AIAGENT", "AI Agent"), "AIAgent", "ai_agent", &obj.aiAgent.enabled, true, [&]() {
            drawStandardComponentMenu(
                "ai_agent",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::AIAgent,
                [&]() { obj.aiAgent = AIAgentComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::AIAgent; inspectorClipboard.aiAgent = obj.aiAgent; },
                [&]() { obj.hasAIAgent = true; obj.aiAgent = inspectorClipboard.aiAgent; changed = true; },
                [&]() { obj.aiAgent = inspectorClipboard.aiAgent; changed = true; },
                removeAgent);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("AIAgent");
            if (ImGui::Button("Open AI Pathfinding")) {
                showAIPathfindingWindow = true;
                aiPreviewAgentId = obj.id;
            }
            bool agentFieldsOpen = beginCompFields("##Fields_AIAgent");
            if (agentFieldsOpen) {
                if (boolRow(Loc::T("COMPONENT_AIAGENT_USE_TARGET", "Use Target"), &obj.aiAgent.useTargetObject)) { changed = true; }
                if (obj.aiAgent.useTargetObject) {
                    endCompFields();
                    agentFieldsOpen = false;
                    if (drawObjectReferenceSlot("##AIAgentTarget", "Target Object", obj.aiAgent.targetId, obj.id, "Target Object")) {
                        aiPreviewTargetId = obj.aiAgent.targetId;
                        changed = true;
                    }
                    agentFieldsOpen = beginCompFields("##Fields_AIAgent2");
                }
                if (agentFieldsOpen) {
                    fieldRow(Loc::T("COMPONENT_AIAGENT_DESTINATION", "Destination"));
                    if (ImGui::DragFloat3("##Destination", &obj.aiAgent.destination.x, 0.05f, -10000.0f, 10000.0f, "%.2f")) {
                        changed = true;
                    }
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1);
                    if (ImGui::Button("Set To Current")) {
                        obj.aiAgent.destination = obj.position;
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_AIAGENT_SPEED", "Speed"));
                    if (ImGui::DragFloat("##Speed", &obj.aiAgent.speed, 0.05f, 0.05f, 100.0f, "%.2f")) {
                        obj.aiAgent.speed = std::max(0.05f, obj.aiAgent.speed);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_AIAGENT_STOP_DISTANCE", "Stop Distance"));
                    if (ImGui::DragFloat("##StoppingDist", &obj.aiAgent.stoppingDistance, 0.01f, 0.0f, 25.0f, "%.2f")) {
                        obj.aiAgent.stoppingDistance = std::clamp(obj.aiAgent.stoppingDistance, 0.0f, 25.0f);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_AIAGENT_REPATH_INTERVAL", "Repath Interval"));
                    if (ImGui::DragFloat("##RepathInterval", &obj.aiAgent.repathInterval, 0.05f, 0.05f, 10.0f, "%.2f")) {
                        obj.aiAgent.repathInterval = std::clamp(obj.aiAgent.repathInterval, 0.05f, 10.0f);
                        changed = true;
                    }
                    if (boolRow(Loc::T("COMPONENT_AIAGENT_AUTO_REPATH", "Auto Repath"), &obj.aiAgent.autoRepath)) { changed = true; }
                    if (boolRow(Loc::T("COMPONENT_AIAGENT_ALIGN_TO_PATH", "Align To Path"), &obj.aiAgent.alignToPath)) { changed = true; }
                    if (boolRow(Loc::T("COMPONENT_AIAGENT_DEBUG_DRAW_PATH", "Debug Draw Path"), &obj.aiAgent.debugDrawPath)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_AIAGENT_TURN_SPEED", "Turn Speed"));
                    if (ImGui::DragFloat("##TurnSpeed", &obj.aiAgent.turnSpeed, 5.0f, 0.0f, 3600.0f, "%.0f deg/s")) {
                        obj.aiAgent.turnSpeed = std::clamp(obj.aiAgent.turnSpeed, 0.0f, 3600.0f);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_AIAGENT_AVOIDANCE_PADDING", "Avoidance Padding"));
                    if (ImGui::DragFloat("##AvoidPad", &obj.aiAgent.avoidancePadding, 0.01f, 0.0f, 10.0f, "%.2f")) {
                        obj.aiAgent.avoidancePadding = std::clamp(obj.aiAgent.avoidancePadding, 0.0f, 10.0f);
                        changed = true;
                    }
                    endCompFields();
                }
            }
            ImGui::PopID();
        }
        if (removeAgent) {
            obj.hasAIAgent = false;
            obj.aiAgent = AIAgentComponent{};
            aiAgentRuntimeStates.erase(obj.id);
            changed = true;
        }
        if (changed) {
            agentSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "offmesh_link" && obj.hasOffMeshLink && sharedOffMeshLink) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.45f, 0.36f, 0.58f, 1.0f));
        bool removeLink = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_OFF_MESH_LINK", "OffMeshLink"), "OffMeshLink", "offmesh_link", &obj.offMeshLink.enabled, true, [&]() {
            drawStandardComponentMenu(
                "offmesh_link",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::OffMeshLink,
                [&]() { obj.offMeshLink = OffMeshLinkComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::OffMeshLink; inspectorClipboard.offMeshLink = obj.offMeshLink; },
                [&]() { obj.hasOffMeshLink = true; obj.offMeshLink = inspectorClipboard.offMeshLink; changed = true; },
                [&]() { obj.offMeshLink = inspectorClipboard.offMeshLink; changed = true; },
                removeLink);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("OffMeshLink");
            if (ImGui::Button("Open AI Pathfinding")) {
                showAIPathfindingWindow = true;
            }
            if (beginCompFields("##Fields_OffMeshLink")) {
                fieldRow(Loc::T("COMPONENT_OFF_MESH_LINK_START", "Start"));
                if (ImGui::DragFloat3("##LinkStart", &obj.offMeshLink.startPoint.x, 0.05f, -10000.0f, 10000.0f, "%.2f")) {
                    changed = true;
                }
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1);
                if (ImGui::Button("Set Start To Object Position")) {
                    obj.offMeshLink.startPoint = obj.position;
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_OFF_MESH_LINK_END", "End"));
                if (ImGui::DragFloat3("##LinkEnd", &obj.offMeshLink.endPoint.x, 0.05f, -10000.0f, 10000.0f, "%.2f")) {
                    changed = true;
                }
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1);
                if (ImGui::Button("Set End To Object Position")) {
                    obj.offMeshLink.endPoint = obj.position;
                    changed = true;
                }
                if (boolRow(Loc::T("COMPONENT_OFF_MESH_LINK_BIDIRECTIONAL", "Bidirectional"), &obj.offMeshLink.bidirectional)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_OFF_MESH_LINK_COST_OVERRIDE", "Cost Override"));
                if (ImGui::DragFloat("##LinkCost", &obj.offMeshLink.costOverride, 0.05f, 0.0f, 1000.0f, "%.2f")) {
                    obj.offMeshLink.costOverride = std::max(0.0f, obj.offMeshLink.costOverride);
                    changed = true;
                }
                noteRow("0 cost = use planar distance. Links snap to nearest walkable cell at each endpoint.");
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeLink) {
            obj.hasOffMeshLink = false;
            obj.offMeshLink = OffMeshLinkComponent{};
            changed = true;
        }
        if (changed) {
            agentSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "animation" && obj.hasAnimation && sharedAnimation) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.4f, 0.35f, 0.55f, 1.0f));
        bool removeAnimation = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_ANIMATION", "Animation"), "Animation", "animation", &obj.animation.enabled, true, [&]() {
            if (ImGui::MenuItem("Reset Component Values")) {
                obj.animation = AnimationComponent{};
                changed = true;
            }
            if (ImGui::MenuItem("View Object Animations in Animator")) {
                showAnimationWindow = true;
                animationTargetId = obj.id;
            }
            drawClipboardMenus(
                "Copy Animator Values",
                "Paste Animator Values as New",
                "Paste Animator Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Animation,
                [&]() {
                    inspectorClipboard.kind = InspectorClipboardKind::Animation;
                    inspectorClipboard.animation = obj.animation;
                },
                [&]() {
                    obj.hasAnimation = true;
                    obj.animation = inspectorClipboard.animation;
                    changed = true;
                },
                [&]() {
                    obj.animation = inspectorClipboard.animation;
                    changed = true;
                });
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeAnimation = true;
            }
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Animation");
            NormalizeAnimationClipSlots(obj.animation);
            if (ImGui::Button("Open Animator")) {
                showAnimationWindow = true;
                animationTargetId = obj.id;
            }
            ImGui::SameLine();
            if (!obj.animation.clips.empty()) {
                int activeIndex = AnimationGetActiveClipIndex(obj.animation);
                const char* preview = (activeIndex >= 0 && activeIndex < static_cast<int>(obj.animation.clips.size()))
                    ? obj.animation.clips[activeIndex].name.c_str()
                    : "<none>";
                if (ImGui::BeginCombo("##AnimActiveClip", preview)) {
                    for (int i = 0; i < static_cast<int>(obj.animation.clips.size()); ++i) {
                        const bool selected = (i == activeIndex);
                        const char* clipName = obj.animation.clips[i].name.empty()
                            ? "<unnamed>"
                            : obj.animation.clips[i].name.c_str();
                        if (ImGui::Selectable(clipName, selected)) {
                            obj.animation.activeClipIndex = i;
                            NormalizeAnimationClipSlots(obj.animation);
                            changed = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("No clips");
            }

            size_t displayKeyCount = obj.animation.keyframes.size();
            size_t displayTrackCount = obj.animation.tracks.size();
            const std::string activeClipAssetPath = AnimationGetActiveClipAssetPath(obj.animation);
            if (!activeClipAssetPath.empty()) {
                auto resolveClipPath = [&](const std::string& storedPath) -> fs::path {
                    if (storedPath.empty()) return {};
                    fs::path p = storedPath;
                    if (p.is_absolute()) return p;
                    if (projectManager.currentProject.isLoaded && !projectManager.currentProject.projectPath.empty()) {
                        return projectManager.currentProject.projectPath / p;
                    }
                    return p;
                };
                auto gatherClipStats = [](const fs::path& clipPath, size_t& outTracks, size_t& outKeys) -> bool {
                    std::ifstream in(clipPath);
                    if (!in.is_open()) return false;
                    outTracks = 0;
                    outKeys = 0;
                    std::string token;
                    while (in >> token) {
                        if (token == "track") {
                            std::string propertyId;
                            int visible = 1;
                            int locked = 0;
                            in >> std::quoted(propertyId) >> visible >> locked;
                            (void)visible;
                            (void)locked;
                            ++outTracks;
                        } else if (token == "keyCount") {
                            size_t keyCount = 0;
                            in >> keyCount;
                            outKeys += keyCount;
                        } else {
                            std::string discard;
                            std::getline(in, discard);
                        }
                    }
                    return !in.bad();
                };
                const fs::path clipPath = resolveClipPath(activeClipAssetPath);
                size_t clipTrackCount = 0;
                size_t clipKeyCount = 0;
                if (!clipPath.empty() && gatherClipStats(clipPath, clipTrackCount, clipKeyCount)) {
                    displayTrackCount = clipTrackCount;
                    displayKeyCount = clipKeyCount;
                }
            }
            ImGui::TextDisabled("Clips: %zu", obj.animation.clips.size());
            ImGui::TextDisabled("Keyframes: %zu | Tracks: %zu", displayKeyCount, displayTrackCount);

            if (beginCompFields("##Fields_Animation")) {
                fieldRow(Loc::T("COMPONENT_ANIMATION_CLIP_LENGTH", "Clip Length"));
                if (ImGui::DragFloat("##ClipLength", &obj.animation.clipLength, 0.05f, 0.1f, 120.0f, "%.2f")) {
                    obj.animation.clipLength = std::max(0.1f, obj.animation.clipLength);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_ANIMATION_PLAY_SPEED", "Play Speed"));
                if (ImGui::DragFloat("##PlaySpeed", &obj.animation.playSpeed, 0.05f, 0.05f, 8.0f, "%.2f")) {
                    obj.animation.playSpeed = std::max(0.05f, obj.animation.playSpeed);
                    changed = true;
                }
                if (boolRow(Loc::T("COMPONENT_ANIMATION_LOOP", "Loop"), &obj.animation.loop)) { changed = true; }
                if (boolRow(Loc::T("COMPONENT_ANIMATION_PLAY_ON_AWAKE", "Play On Awake"), &obj.animation.playOnAwake)) { changed = true; }
                if (boolRow(Loc::T("COMPONENT_ANIMATION_APPLY_ON_SCRUB", "Apply On Scrub"), &obj.animation.applyOnScrub)) { changed = true; }
                endCompFields();
            }

            if (ImGui::Button("Clear Keyframes")) {
                obj.animation.keyframes.clear();
                changed = true;
            }

            ImGui::PopID();
        }
        if (removeAnimation) {
            obj.hasAnimation = false;
            obj.animation = AnimationComponent{};
            changed = true;
        }
        if (changed) {
            animationSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "skeletal_animation" && obj.hasSkeletalAnimation && sharedSkeletal) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.4f, 0.6f, 1.0f));
        bool removeSkeletal = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_SKELETAL", "Skeletal"), "Skeletal", "skeletal_animation", &obj.skeletal.enabled, true, [&]() {
            drawStandardComponentMenu(
                "skeletal_animation",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Skeletal,
                [&]() { obj.skeletal = SkeletalAnimationComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::Skeletal; inspectorClipboard.skeletal = obj.skeletal; },
                [&]() { obj.hasSkeletalAnimation = true; obj.skeletal = inspectorClipboard.skeletal; changed = true; },
                [&]() { obj.skeletal = inspectorClipboard.skeletal; changed = true; },
                removeSkeletal);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Skeletal");
            if (beginCompFields("##Fields_Skeletal")) {
                std::string err;
                const ModelSceneData* sceneData =
                    obj.meshPath.empty() ? nullptr
                                         : getModelLoader().loadModelSceneCached(obj.meshPath, err);
                bool hasClips = sceneData && !sceneData->animations.empty();
                if (hasClips) {
                    std::vector<const char*> clipNames;
                    clipNames.reserve(sceneData->animations.size());
                    for (const auto& clip : sceneData->animations) {
                        clipNames.push_back(clip.name.c_str());
                    }
                    int clipIndex = std::clamp(obj.skeletal.clipIndex, 0, (int)clipNames.size() - 1);
                    fieldRow(Loc::T("COMPONENT_SKELETAL_CLIP", "Clip"));
                    if (ImGui::Combo("##Clip", &clipIndex, clipNames.data(), (int)clipNames.size())) {
                        obj.skeletal.clipIndex = clipIndex;
                        obj.skeletal.time = 0.0f;
                        changed = true;
                    }
                } else {
                    noteRow("No animation clips found");
                }
                if (boolRow(Loc::T("COMPONENT_SKELETAL_USE_ANIMATION", "Use Animation"), &obj.skeletal.useAnimation)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_SKELETAL_PLAY_SPEED", "Play Speed"));
                if (ImGui::DragFloat("##PlaySpeed", &obj.skeletal.playSpeed, 0.05f, 0.05f, 8.0f, "%.2f")) {
                    obj.skeletal.playSpeed = std::max(0.05f, obj.skeletal.playSpeed);
                    changed = true;
                }
                if (boolRow(Loc::T("COMPONENT_SKELETAL_LOOP", "Loop"), &obj.skeletal.loop)) { changed = true; }
                if (boolRow(Loc::T("COMPONENT_SKELETAL_GPU_SKINNING", "GPU Skinning"), &obj.skeletal.useGpuSkinning)) { changed = true; }
                if (boolRow(Loc::T("COMPONENT_SKELETAL_CPU_FALLBACK", "CPU Fallback"), &obj.skeletal.allowCpuFallback)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_SKELETAL_MAX_BONES", "Max Bones"));
                if (ImGui::DragInt("##MaxBones", &obj.skeletal.maxBones, 1, 8, 128)) {
                    obj.skeletal.maxBones = std::clamp(obj.skeletal.maxBones, 8, 128);
                    changed = true;
                }
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeSkeletal) {
            obj.hasSkeletalAnimation = false;
            obj.skeletal = SkeletalAnimationComponent{};
            changed = true;
        }
        if (changed) {
            skeletalSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    {
        const auto drawXRComponentMenu = [&](const std::string& key, const auto& onReset,
                                             bool& removeFlag) {
            drawReorderMenuItems(key);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Component Values")) {
                onReset();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                removeFlag = true;
            }
        };

        if (inspectorComponentKey == "xr_origin" && obj.hasXROrigin && sharedXROrigin) {
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.42f, 0.58f, 1.0f));
            bool removeComponent = false;
            bool changed = false;
            auto header = drawComponentHeader(Loc::T("COMPONENT_XR_ORIGIN", "XR Origin"), "XROrigin",
                                              "xr_origin", &obj.xrOrigin.enabled, true, [&]() {
                drawXRComponentMenu("xr_origin",
                                    [&]() { obj.xrOrigin = XROriginComponent{}; changed = true; },
                                    removeComponent);
            });
            if (header.enabledChanged) changed = true;
            if (header.open) {
                InspectorBodyScope _ibs(*this);
                ImGui::PushID("XROrigin");
                XROriginComponent& origin = obj.xrOrigin;
                ImGui::TextDisabled("Move this object to move the player through the world.");
                if (beginCompFields("##Fields_XROrigin")) {
                    const char* modes[] = { "Follow Project Settings", "Floor", "Eye Level" };
                    int modeIndex = static_cast<int>(origin.trackingOriginMode);
                    fieldRow(Loc::T("COMPONENT_XR_ORIGIN_MODE", "Tracking Origin"));
                    if (ImGui::Combo("##XROriginMode", &modeIndex, modes, IM_ARRAYSIZE(modes))) {
                        origin.trackingOriginMode = static_cast<XROriginComponent::Mode>(modeIndex);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_XR_ORIGIN_RIG_SCALE", "Rig Scale"));
                    if (ImGui::DragFloat("##XRRigScale", &origin.rigScale, 0.01f, 0.01f, 100.0f, "%.2f")) {
                        origin.rigScale = std::clamp(origin.rigScale, 0.01f, 100.0f);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_XR_ORIGIN_CAMERA_Y", "Camera Y Offset"));
                    if (ImGui::DragFloat("##XRCameraYOffset", &origin.cameraYOffset, 0.01f, -10.0f, 10.0f, "%.2f")) {
                        changed = true;
                    }
                    endCompFields();
                }
                if (origin.trackingOriginMode == XROriginComponent::Mode::EyeLevel) {
                    ImGui::TextWrapped("%s", "Eye Level puts the origin at the recentre pose. Use Camera Y "
                                 "Offset to lift a seated player off the floor plane.");
                }
                ImGui::PopID();
            }
            if (removeComponent) { obj.hasXROrigin = false; changed = true; }
            if (changed) projectManager.currentProject.hasUnsavedChanges = true;
            ImGui::PopStyleColor();
        }

        if (inspectorComponentKey == "xr_camera" && obj.hasXRCamera && sharedXRCamera) {
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.42f, 0.58f, 1.0f));
            bool removeComponent = false;
            bool changed = false;
            auto header = drawComponentHeader(Loc::T("COMPONENT_XR_CAMERA", "XR Camera"), "XRCamera",
                                              "xr_camera", &obj.xrCamera.enabled, true, [&]() {
                drawXRComponentMenu("xr_camera",
                                    [&]() { obj.xrCamera = XRCameraComponent{}; changed = true; },
                                    removeComponent);
            });
            if (header.enabledChanged) changed = true;
            if (header.open) {
                InspectorBodyScope _ibs(*this);
                ImGui::PushID("XRCamera");
                XRCameraComponent& cam = obj.xrCamera;
                if (!obj.hasCamera) {
                    ImGui::TextWrapped("%s", "This object has no Camera component, so there is nothing for "
                                 "the headset to render through. Add one.");
                }
                if (beginCompFields("##Fields_XRCamera")) {
                    fieldRow(Loc::T("COMPONENT_XR_CAMERA_APPLY", "Apply Tracking"));
                    if (ImGui::Checkbox("##XRCameraApply", &cam.applyTracking)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_CAMERA_TRACK_POS", "Track Position"));
                    if (ImGui::Checkbox("##XRCameraTrackPos", &cam.trackPosition)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_CAMERA_TRACK_ROT", "Track Rotation"));
                    if (ImGui::Checkbox("##XRCameraTrackRot", &cam.trackRotation)) changed = true;
                    endCompFields();
                }
                ImGui::PopID();
            }
            if (removeComponent) { obj.hasXRCamera = false; changed = true; }
            if (changed) projectManager.currentProject.hasUnsavedChanges = true;
            ImGui::PopStyleColor();
        }

        if (inspectorComponentKey == "xr_controller" && obj.hasXRController && sharedXRController) {
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.42f, 0.58f, 1.0f));
            bool removeComponent = false;
            bool changed = false;
            auto header = drawComponentHeader(Loc::T("COMPONENT_XR_CONTROLLER", "XR Controller"),
                                              "XRController", "xr_controller",
                                              &obj.xrController.enabled, true, [&]() {
                drawXRComponentMenu("xr_controller",
                                    [&]() { obj.xrController = XRControllerComponent{}; changed = true; },
                                    removeComponent);
            });
            if (header.enabledChanged) changed = true;
            if (header.open) {
                InspectorBodyScope _ibs(*this);
                ImGui::PushID("XRController");
                XRControllerComponent& controller = obj.xrController;
                if (beginCompFields("##Fields_XRController")) {
                    const char* hands[] = { "Left", "Right" };
                    int handIndex = static_cast<int>(controller.hand);
                    fieldRow(Loc::T("COMPONENT_XR_CONTROLLER_HAND", "Hand"));
                    if (ImGui::Combo("##XRControllerHand", &handIndex, hands, IM_ARRAYSIZE(hands))) {
                        controller.hand = (handIndex == 1) ? XRHand::Right : XRHand::Left;
                        changed = true;
                    }
                    const char* poses[] = { "Grip Pose", "Aim Pose" };
                    int poseIndex = static_cast<int>(controller.poseSource);
                    fieldRow(Loc::T("COMPONENT_XR_CONTROLLER_POSE", "Pose Source"));
                    if (ImGui::Combo("##XRControllerPose", &poseIndex, poses, IM_ARRAYSIZE(poses))) {
                        controller.poseSource = (poseIndex == 1) ? XRControllerPoseSource::Aim
                                                                 : XRControllerPoseSource::Grip;
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_XR_CONTROLLER_TRACK_POS", "Track Position"));
                    if (ImGui::Checkbox("##XRControllerTrackPos", &controller.trackPosition)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_CONTROLLER_TRACK_ROT", "Track Rotation"));
                    if (ImGui::Checkbox("##XRControllerTrackRot", &controller.trackRotation)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_CONTROLLER_HIDE", "Hide When Not Tracked"));
                    if (ImGui::Checkbox("##XRControllerHide", &controller.hideWhenNotTracked)) changed = true;
                    endCompFields();
                }
                ImGui::TextWrapped("%s", controller.poseSource == XRControllerPoseSource::Aim
                                             ? "Aim Pose points where the controller is aiming. "
                                               "Use it for rays, weapons and UI targeting."
                                             : "Grip Pose follows the physical controller. Use it "
                                               "for hand models and grabbing.");
                ImGui::PopID();
            }
            if (removeComponent) { obj.hasXRController = false; changed = true; }
            if (changed) projectManager.currentProject.hasUnsavedChanges = true;
            ImGui::PopStyleColor();
        }

        if (inspectorComponentKey == "xr_action_controller" && obj.hasXRActionBasedController && sharedXRActionController) {
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.42f, 0.58f, 1.0f));
            bool removeComponent = false;
            bool changed = false;
            auto header = drawComponentHeader(
                Loc::T("COMPONENT_XR_ACTION_CONTROLLER", "XR Action-Based Controller"),
                "XRActionController", "xr_action_controller",
                &obj.xrActionBasedController.enabled, true, [&]() {
                    drawXRComponentMenu("xr_action_controller",
                                        [&]() { obj.xrActionBasedController = XRActionBasedControllerComponent{}; changed = true; },
                                        removeComponent);
                });
            if (header.enabledChanged) changed = true;
            if (header.open) {
                InspectorBodyScope _ibs(*this);
                ImGui::PushID("XRActionController");
                XRActionBasedControllerComponent& c = obj.xrActionBasedController;
                const char* buttons[] = { "Trigger", "Grip", "Primary Button",
                                          "Secondary Button", "Thumbstick Click", "Menu" };
                const auto buttonCombo = [&](const char* label, const char* id, int& value) {
                    fieldRow(label);
                    int index = std::clamp(value, 0, IM_ARRAYSIZE(buttons) - 1);
                    if (ImGui::Combo(id, &index, buttons, IM_ARRAYSIZE(buttons))) {
                        value = index;
                        changed = true;
                    }
                };
                if (beginCompFields("##Fields_XRActionController")) {
                    const char* hands[] = { "Left", "Right" };
                    int handIndex = static_cast<int>(c.hand);
                    fieldRow(Loc::T("COMPONENT_XR_ACTION_HAND", "Hand"));
                    if (ImGui::Combo("##XRActionHand", &handIndex, hands, IM_ARRAYSIZE(hands))) {
                        c.hand = (handIndex == 1) ? XRHand::Right : XRHand::Left;
                        changed = true;
                    }
                    buttonCombo(Loc::T("COMPONENT_XR_ACTION_SELECT", "Select"), "##XRActionSelect", c.selectButton);
                    buttonCombo(Loc::T("COMPONENT_XR_ACTION_ACTIVATE", "Activate"), "##XRActionActivate", c.activateButton);
                    buttonCombo(Loc::T("COMPONENT_XR_ACTION_UI", "UI Press"), "##XRActionUi", c.uiPressButton);
                    fieldRow(Loc::T("COMPONENT_XR_ACTION_HAPTICS", "Enable Haptics"));
                    if (ImGui::Checkbox("##XRActionHaptics", &c.enableHaptics)) changed = true;
                    endCompFields();
                }
                if (c.enableHaptics && beginCompFields("##Fields_XRActionHaptics")) {
                    fieldRow(Loc::T("COMPONENT_XR_ACTION_AMPLITUDE", "Amplitude"));
                    if (ImGui::SliderFloat("##XRActionAmplitude", &c.hapticAmplitude, 0.0f, 1.0f, "%.2f")) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_ACTION_DURATION", "Duration"));
                    if (ImGui::DragFloat("##XRActionDuration", &c.hapticDuration, 0.01f, 0.0f, 5.0f, "%.2f s")) {
                        c.hapticDuration = std::clamp(c.hapticDuration, 0.0f, 5.0f);
                        changed = true;
                    }
                    endCompFields();
                }
                ImGui::PopID();
            }
            if (removeComponent) { obj.hasXRActionBasedController = false; changed = true; }
            if (changed) projectManager.currentProject.hasUnsavedChanges = true;
            ImGui::PopStyleColor();
        }

        if (inspectorComponentKey == "xr_ray_interactor" && obj.hasXRRayInteractor && sharedXRRayInteractor) {
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.42f, 0.58f, 1.0f));
            bool removeComponent = false;
            bool changed = false;
            auto header = drawComponentHeader(Loc::T("COMPONENT_XR_RAY", "XR Ray Interactor"),
                                              "XRRayInteractor", "xr_ray_interactor",
                                              &obj.xrRayInteractor.enabled, true, [&]() {
                drawXRComponentMenu("xr_ray_interactor",
                                    [&]() { obj.xrRayInteractor = XRRayInteractorComponent{}; changed = true; },
                                    removeComponent);
            });
            if (header.enabledChanged) changed = true;
            if (header.open) {
                InspectorBodyScope _ibs(*this);
                ImGui::PushID("XRRayInteractor");
                XRRayInteractorComponent& ray = obj.xrRayInteractor;
                if (obj.hasXRController && obj.xrController.poseSource != XRControllerPoseSource::Aim) {
                    ImGui::TextWrapped("%s", "This object's XR Controller uses the Grip Pose, so the ray will "
                                 "fire from the hand rather than from where the controller points. "
                                 "Set Pose Source to Aim Pose.");
                }
                if (beginCompFields("##Fields_XRRay")) {
                    const char* rayTypes[] = { "Straight" };
                    int typeIndex = 0;
                    fieldRow(Loc::T("COMPONENT_XR_RAY_TYPE", "Ray Type"));
                    ImGui::BeginDisabled(true);
                    ImGui::Combo("##XRRayType", &typeIndex, rayTypes, IM_ARRAYSIZE(rayTypes));
                    ImGui::EndDisabled();
                    fieldRow(Loc::T("COMPONENT_XR_RAY_MAX_DISTANCE", "Max Distance"));
                    if (ImGui::DragFloat("##XRRayMaxDistance", &ray.maxDistance, 0.1f, 0.01f, 1000.0f, "%.2f")) {
                        ray.maxDistance = std::max(0.01f, ray.maxDistance);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_XR_RAY_UI", "UI Interaction"));
                    if (ImGui::Checkbox("##XRRayUi", &ray.uiInteraction)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_RAY_LINE", "Line Visual"));
                    if (ImGui::Checkbox("##XRRayLine", &ray.showLineVisual)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_RAY_COLOR", "Line Color"));
                    if (ImGui::ColorEdit4("##XRRayColor", &ray.lineColor.r)) changed = true;
                    endCompFields();
                }
                if (ray.uiInteraction) {
                    ImGui::TextWrapped("%s", "ModuGUI raycasting from an XR ray is not implemented yet; this "
                                 "flag is stored but does nothing.");
                }
                ImGui::PopID();
            }
            if (removeComponent) { obj.hasXRRayInteractor = false; changed = true; }
            if (changed) projectManager.currentProject.hasUnsavedChanges = true;
            ImGui::PopStyleColor();
        }

        if (inspectorComponentKey == "xr_direct_interactor" && obj.hasXRDirectInteractor && sharedXRDirectInteractor) {
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.42f, 0.58f, 1.0f));
            bool removeComponent = false;
            bool changed = false;
            auto header = drawComponentHeader(Loc::T("COMPONENT_XR_DIRECT", "XR Direct Interactor"),
                                              "XRDirectInteractor", "xr_direct_interactor",
                                              &obj.xrDirectInteractor.enabled, true, [&]() {
                drawXRComponentMenu("xr_direct_interactor",
                                    [&]() { obj.xrDirectInteractor = XRDirectInteractorComponent{}; changed = true; },
                                    removeComponent);
            });
            if (header.enabledChanged) changed = true;
            if (header.open) {
                InspectorBodyScope _ibs(*this);
                ImGui::PushID("XRDirectInteractor");
                XRDirectInteractorComponent& direct = obj.xrDirectInteractor;
                if (beginCompFields("##Fields_XRDirect")) {
                    fieldRow(Loc::T("COMPONENT_XR_DIRECT_RADIUS", "Interaction Radius"));
                    if (ImGui::DragFloat("##XRDirectRadius", &direct.interactionRadius, 0.005f, 0.001f, 5.0f, "%.3f")) {
                        direct.interactionRadius = std::max(0.001f, direct.interactionRadius);
                        changed = true;
                    }
                    endCompFields();
                }
                ImGui::PopID();
            }
            if (removeComponent) { obj.hasXRDirectInteractor = false; changed = true; }
            if (changed) projectManager.currentProject.hasUnsavedChanges = true;
            ImGui::PopStyleColor();
        }

        if (inspectorComponentKey == "xr_grab_interactable" && obj.hasXRGrabInteractable && sharedXRGrabInteractable) {
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.30f, 0.42f, 0.58f, 1.0f));
            bool removeComponent = false;
            bool changed = false;
            auto header = drawComponentHeader(Loc::T("COMPONENT_XR_GRAB", "XR Grab Interactable"),
                                              "XRGrabInteractable", "xr_grab_interactable",
                                              &obj.xrGrabInteractable.enabled, true, [&]() {
                drawXRComponentMenu("xr_grab_interactable",
                                    [&]() { obj.xrGrabInteractable = XRGrabInteractableComponent{}; changed = true; },
                                    removeComponent);
            });
            if (header.enabledChanged) changed = true;
            if (header.open) {
                InspectorBodyScope _ibs(*this);
                ImGui::PushID("XRGrabInteractable");
                XRGrabInteractableComponent& grab = obj.xrGrabInteractable;
                if (beginCompFields("##Fields_XRGrab")) {
                    const char* movement[] = { "Instant", "Kinematic", "Velocity Tracking" };
                    int movementIndex = static_cast<int>(grab.movementType);
                    fieldRow(Loc::T("COMPONENT_XR_GRAB_MOVEMENT", "Movement Type"));
                    if (ImGui::Combo("##XRGrabMovement", &movementIndex, movement, IM_ARRAYSIZE(movement))) {
                        grab.movementType = static_cast<XRGrabInteractableComponent::MovementType>(movementIndex);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_XR_GRAB_LEFT", "Allow Left Hand"));
                    if (ImGui::Checkbox("##XRGrabLeft", &grab.allowLeftHand)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_GRAB_RIGHT", "Allow Right Hand"));
                    if (ImGui::Checkbox("##XRGrabRight", &grab.allowRightHand)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_GRAB_TRACK_POS", "Track Position"));
                    if (ImGui::Checkbox("##XRGrabTrackPos", &grab.trackPosition)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_GRAB_TRACK_ROT", "Track Rotation"));
                    if (ImGui::Checkbox("##XRGrabTrackRot", &grab.trackRotation)) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_GRAB_THROW", "Throw On Detach"));
                    if (ImGui::Checkbox("##XRGrabThrow", &grab.throwOnDetach)) changed = true;
                    endCompFields();
                }
                if (grab.throwOnDetach && beginCompFields("##Fields_XRGrabThrow")) {
                    fieldRow(Loc::T("COMPONENT_XR_GRAB_THROW_SCALE", "Throw Velocity Scale"));
                    if (ImGui::DragFloat("##XRGrabThrowScale", &grab.throwVelocityScale, 0.05f, 0.0f, 10.0f, "%.2f")) changed = true;
                    fieldRow(Loc::T("COMPONENT_XR_GRAB_THROW_ANGULAR", "Throw Angular Scale"));
                    if (ImGui::DragFloat("##XRGrabThrowAngular", &grab.throwAngularVelocityScale, 0.05f, 0.0f, 10.0f, "%.2f")) changed = true;
                    endCompFields();
                }
                if (grab.movementType == XRGrabInteractableComponent::MovementType::VelocityTracking &&
                    !obj.hasRigidbody) {
                    ImGui::TextWrapped("%s", "Velocity Tracking drives a rigidbody, and this object has none, "
                                 "so it falls back to moving the transform directly. Add a "
                                 "Rigidbody for it to collide while held or be thrown.");
                }
                ImGui::PopID();
            }
            if (removeComponent) { obj.hasXRGrabInteractable = false; changed = true; }
            if (changed) projectManager.currentProject.hasUnsavedChanges = true;
            ImGui::PopStyleColor();
        }
    }

    if (inspectorComponentKey == "reverb_zone" && obj.hasReverbZone && sharedReverb) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.4f, 0.45f, 0.6f, 1.0f));
        bool removeReverbZone = false;
        bool changed = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_REVERB_ZONE", "Reverb Zone"), "ReverbZone", "reverb_zone", &obj.reverbZone.enabled, true, [&]() {
            drawStandardComponentMenu(
                "reverb_zone",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::ReverbZone,
                [&]() { obj.reverbZone = ReverbZoneComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::ReverbZone; inspectorClipboard.reverbZone = obj.reverbZone; },
                [&]() { obj.hasReverbZone = true; obj.reverbZone = inspectorClipboard.reverbZone; changed = true; },
                [&]() { obj.reverbZone = inspectorClipboard.reverbZone; changed = true; },
                removeReverbZone);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("ReverbZone");
            auto& zone = obj.reverbZone;

            if (beginCompFields("##Fields_ReverbZone")) {
                const char* presets[] = { "Room", "Living Room", "Hall", "Forest", "Custom" };
                int presetIndex = static_cast<int>(zone.preset);
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_PRESET", "Preset"));
                if (ImGui::Combo("##Preset", &presetIndex, presets, IM_ARRAYSIZE(presets))) {
                    ApplyReverbPreset(zone, static_cast<ReverbPreset>(presetIndex));
                    changed = true;
                }

                const char* shapes[] = { "Box", "Sphere" };
                int shapeIndex = static_cast<int>(zone.shape);
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_SHAPE", "Shape"));
                if (ImGui::Combo("##Shape", &shapeIndex, shapes, IM_ARRAYSIZE(shapes))) {
                    zone.shape = static_cast<ReverbZoneShape>(shapeIndex);
                    changed = true;
                }

                if (zone.shape == ReverbZoneShape::Sphere) {
                    fieldRow(Loc::T("COMPONENT_REVERB_ZONE_RADIUS", "Radius"));
                    if (ImGui::DragFloat("##Radius", &zone.radius, 0.1f, 0.1f, 500.0f, "%.2f")) {
                        zone.radius = std::max(0.1f, zone.radius);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_REVERB_ZONE_MIN_DISTANCE", "Min Distance"));
                    if (ImGui::DragFloat("##MinDist", &zone.minDistance, 0.05f, 0.0f, 500.0f, "%.2f")) {
                        zone.minDistance = std::max(0.0f, zone.minDistance);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_REVERB_ZONE_MAX_DISTANCE", "Max Distance"));
                    if (ImGui::DragFloat("##MaxDist", &zone.maxDistance, 0.05f, zone.minDistance + 0.1f, 1000.0f, "%.2f")) {
                        zone.maxDistance = std::max(zone.maxDistance, zone.minDistance + 0.1f);
                        changed = true;
                    }
                } else {
                    fieldRow(Loc::T("COMPONENT_REVERB_ZONE_BOX_SIZE", "Box Size"));
                    if (ImGui::DragFloat3("##BoxSize", &zone.boxSize.x, 0.1f, 0.1f, 500.0f, "%.2f")) {
                        zone.boxSize = glm::max(zone.boxSize, glm::vec3(0.1f));
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_REVERB_ZONE_BLEND_DISTANCE", "Blend Distance"));
                    if (ImGui::DragFloat("##BlendDist", &zone.blendDistance, 0.05f, 0.0f, 50.0f, "%.2f")) {
                        zone.blendDistance = std::max(0.0f, zone.blendDistance);
                        changed = true;
                    }
                }

                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_ROOM", "Room"));
                if (ImGui::SliderFloat("##Room", &zone.room, -10000.0f, 0.0f, "%.0f dB")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_ROOM_HF", "Room HF"));
                if (ImGui::SliderFloat("##RoomHF", &zone.roomHF, -10000.0f, 0.0f, "%.0f dB")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_ROOM_LF", "Room LF"));
                if (ImGui::SliderFloat("##RoomLF", &zone.roomLF, -10000.0f, 0.0f, "%.0f dB")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_DECAY_TIME", "Decay Time"));
                if (ImGui::SliderFloat("##DecayTime", &zone.decayTime, 0.1f, 20.0f, "%.2f s")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_DECAY_HF_RATIO", "Decay HF Ratio"));
                if (ImGui::SliderFloat("##DecayHFRatio", &zone.decayHFRatio, 0.1f, 2.0f, "%.2f")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_REFLECTIONS", "Reflections"));
                if (ImGui::SliderFloat("##Reflections", &zone.reflections, -10000.0f, 1000.0f, "%.0f dB")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_REFLECT_DELAY", "Reflect Delay"));
                if (ImGui::SliderFloat("##ReflectDelay", &zone.reflectionsDelay, 0.0f, 0.1f, "%.3f s")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_REVERB", "Reverb"));
                if (ImGui::SliderFloat("##Reverb", &zone.reverb, -10000.0f, 2000.0f, "%.0f dB")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_REVERB_DELAY", "Reverb Delay"));
                if (ImGui::SliderFloat("##ReverbDelay", &zone.reverbDelay, 0.0f, 0.1f, "%.3f s")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_HF_REFERENCE", "HF Reference"));
                if (ImGui::SliderFloat("##HFRef", &zone.hfReference, 1000.0f, 20000.0f, "%.0f Hz")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_LF_REFERENCE", "LF Reference"));
                if (ImGui::SliderFloat("##LFRef", &zone.lfReference, 20.0f, 1000.0f, "%.0f Hz")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_ROOM_ROLLOFF", "Room Rolloff"));
                if (ImGui::SliderFloat("##RoomRolloff", &zone.roomRolloffFactor, 0.0f, 10.0f, "%.2f")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_DIFFUSION", "Diffusion"));
                if (ImGui::SliderFloat("##Diffusion", &zone.diffusion, 0.0f, 100.0f, "%.0f")) { zone.preset = ReverbPreset::Custom; changed = true; }
                fieldRow(Loc::T("COMPONENT_REVERB_ZONE_DENSITY", "Density"));
                if (ImGui::SliderFloat("##Density", &zone.density, 0.0f, 100.0f, "%.0f")) { zone.preset = ReverbPreset::Custom; changed = true; }
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeReverbZone) {
            obj.hasReverbZone = false;
            changed = true;
        }
        if (changed) {
            reverbSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "camera" && obj.hasCamera && sharedCamera) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.45f, 0.35f, 0.65f, 1.0f));
        bool changed = false;
        bool removeCamera = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_CAMERA", "Camera"), "Camera", "camera", nullptr, true, [&]() {
            drawStandardComponentMenu(
                "camera",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Camera,
                [&]() { obj.camera = CameraComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::Camera; inspectorClipboard.camera = obj.camera; },
                [&]() { obj.hasCamera = true; obj.camera = inspectorClipboard.camera; changed = true; },
                [&]() { obj.camera = inspectorClipboard.camera; changed = true; },
                removeCamera);
        });
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Camera");
            if (beginCompFields("##Fields_Camera")) {
                auto sectionRow = [](const char* label) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.82f, 0.86f, 0.94f, 1.0f), "%s", label);
                };

                const char* cameraTypes[] = { "Scene", "Player" };
                int camType = static_cast<int>(obj.camera.type);
                fieldRow(Loc::T("COMPONENT_CAMERA_TYPE", "Type"));
                if (ImGui::Combo("##Type", &camType, cameraTypes, IM_ARRAYSIZE(cameraTypes))) {
                    obj.camera.type = static_cast<SceneCameraType>(camType);
                    changed = true;
                }

                bool project2D = isProject2DPipeline();
                bool project25D = isProject25DPipeline();
                bool cameraUses2D = project2D || obj.camera.use2D;

                sectionRow("Projection");
                if (!cameraUses2D) {
                    const char* projectionModes[] = { "Perspective", "Orthographic" };
                    int projMode = static_cast<int>(obj.camera.projection);
                    fieldRow(Loc::T("COMPONENT_CAMERA_PROJECTION", "Projection"));
                    if (ImGui::Combo("##Projection", &projMode, projectionModes, IM_ARRAYSIZE(projectionModes))) {
                        obj.camera.projection = static_cast<SceneCameraProjection>(projMode);
                        changed = true;
                    }
                }
                const bool orthographic3D = !cameraUses2D &&
                    obj.camera.projection == SceneCameraProjection::Orthographic;
                if (!cameraUses2D && !orthographic3D) {
                    const char* fovAxes[] = { "Vertical", "Horizontal" };
                    int fovAxis = static_cast<int>(obj.camera.fovAxis);
                    fieldRow(Loc::T("COMPONENT_CAMERA_FIELD_OF_VIEW_AXIS", "Field of View Axis"));
                    if (ImGui::Combo("##FovAxis", &fovAxis, fovAxes, IM_ARRAYSIZE(fovAxes))) {
                        obj.camera.fovAxis = static_cast<SceneCameraFovAxis>(fovAxis);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_CAMERA_FIELD_OF_VIEW", "Field of View"));
                    if (ImGui::SliderFloat("##FOV", &obj.camera.fov, 1.0f, 170.0f, "%.0f deg")) { changed = true; }
                } else if (orthographic3D) {
                    fieldRow(Loc::T("COMPONENT_CAMERA_SIZE", "Size"));
                    if (ImGui::DragFloat("##OrthoSize", &obj.camera.orthoSize, 0.1f, 0.01f, 10000.0f, "%.2f")) {
                        obj.camera.orthoSize = std::max(0.01f, obj.camera.orthoSize);
                        changed = true;
                    }
                    noteRow("World-unit half-height of the view volume.");
                }
                fieldRow(Loc::T("COMPONENT_CAMERA_CLIPPING_PLANES_NEAR", "Clipping Planes  Near"));
                if (ImGui::DragFloat("##NearClip", &obj.camera.nearClip, 0.01f, 0.01f, obj.camera.farClip - 0.01f, "%.3f")) {
                    obj.camera.nearClip = std::max(0.01f, std::min(obj.camera.nearClip, obj.camera.farClip - 0.01f));
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_CAMERA_FAR", "Far"));
                if (ImGui::DragFloat("##FarClip", &obj.camera.farClip, 0.1f, obj.camera.nearClip + 0.05f, 1000.0f, "%.1f")) {
                    obj.camera.farClip = std::max(obj.camera.nearClip + 0.05f, obj.camera.farClip);
                    changed = true;
                }

                sectionRow("Rendering");
                if (boolRow(Loc::T("COMPONENT_CAMERA_POST_PROCESSING", "Post Processing"), &obj.camera.applyPostFX)) { changed = true; }
                if (boolRow(Loc::T("COMPONENT_CAMERA_RENDER_SHADOWS", "Render Shadows"), &obj.camera.renderShadows)) { changed = true; }

                {
                    const std::vector<std::string>& layers =
                        projectManager.currentProject.physicsSettings.collisionLayers;
                    const int layerCount = static_cast<int>(std::min<size_t>(layers.size(), 32));
                    uint32_t definedMask = (layerCount >= 32)
                        ? 0xFFFFFFFFu
                        : ((1u << layerCount) - 1u);
                    const uint32_t maskedDefined = obj.camera.cullingMask & definedMask;
                    const char* maskPreview = "Mixed";
                    if (obj.camera.cullingMask == 0xFFFFFFFFu || maskedDefined == definedMask) {
                        maskPreview = "Everything";
                    } else if (maskedDefined == 0u) {
                        maskPreview = "Nothing";
                    }
                    fieldRow(Loc::T("COMPONENT_CAMERA_CULLING_MASK", "Culling Mask"));
                    if (ImGui::BeginCombo("##CullingMask", maskPreview)) {
                        if (ImGui::Selectable("Everything", obj.camera.cullingMask == 0xFFFFFFFFu, ImGuiSelectableFlags_DontClosePopups)) {
                            obj.camera.cullingMask = 0xFFFFFFFFu;
                            changed = true;
                        }
                        if (ImGui::Selectable("Nothing", maskedDefined == 0u, ImGuiSelectableFlags_DontClosePopups)) {
                            obj.camera.cullingMask = 0u;
                            changed = true;
                        }
                        ImGui::Separator();
                        for (int layerIdx = 0; layerIdx < layerCount; ++layerIdx) {
                            bool layerOn = (obj.camera.cullingMask & (1u << layerIdx)) != 0u;
                            std::string layerLabel = layers[static_cast<size_t>(layerIdx)].empty()
                                ? ("Layer " + std::to_string(layerIdx))
                                : layers[static_cast<size_t>(layerIdx)];
                            if (ImGui::Checkbox((layerLabel + "##CullLayer" + std::to_string(layerIdx)).c_str(), &layerOn)) {
                                if (layerOn) obj.camera.cullingMask |= (1u << layerIdx);
                                else obj.camera.cullingMask &= ~(1u << layerIdx);
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }

                sectionRow("Environment");
                {
                    const char* backgroundModes[] = { "Skybox", "Solid Color" };
                    int bgMode = static_cast<int>(obj.camera.background);
                    fieldRow(Loc::T("COMPONENT_CAMERA_BACKGROUND_TYPE", "Background Type"));
                    if (ImGui::Combo("##BackgroundType", &bgMode, backgroundModes, IM_ARRAYSIZE(backgroundModes))) {
                        obj.camera.background = static_cast<SceneCameraBackground>(bgMode);
                        changed = true;
                    }
                    if (obj.camera.background == SceneCameraBackground::SolidColor) {
                        fieldRow(Loc::T("COMPONENT_CAMERA_BACKGROUND", "Background"));
                        if (ImGui::ColorEdit3("##BackgroundColor", &obj.camera.backgroundColor.x)) {
                            changed = true;
                        }
                    }
                }

                if (project2D) {
                    noteRow("2D camera mode is controlled by Project Pipeline.");
                } else {
                    if (project25D) {
                        noteRow("2.5D projects keep perspective cameras by default; use Legacy 2D Cam only when you want an orthographic shot.");
                    }
                    if (boolRow(Loc::T("COMPONENT_CAMERA_LEGACY_2_D_CAM", "Legacy 2D Cam"), &obj.camera.use2D)) { changed = true; }
                }
                if (cameraUses2D) {
                    fieldRow(Loc::T("COMPONENT_CAMERA_PIXELS_UNIT", "Pixels/Unit"));
                    if (ImGui::DragFloat("##PixelsPerUnit", &obj.camera.pixelsPerUnit, 1.0f, 1.0f, 2000.0f, "%.1f")) {
                        obj.camera.pixelsPerUnit = std::max(1.0f, obj.camera.pixelsPerUnit);
                        changed = true;
                    }
                    noteRow("Uses X/Y for 2D view; Z stays fixed.");
                }
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeCamera) {
            obj.hasCamera = false;
            UpdateLegacyTypeFromComponents(obj);
            changed = true;
        }
        if (changed) {
            cameraSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "camera_follow2d" && obj.hasCameraFollow2D && sharedCameraFollow2D) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.55f, 0.4f, 1.0f));
        bool changed = false;
        bool removeFollow = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_CAMERA_FOLLOW2_D", "Camera Follow 2D"), "CameraFollow2D", "camera_follow2d", &obj.cameraFollow2D.enabled, true, [&]() {
            drawStandardComponentMenu(
                "camera_follow2d",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::CameraFollow2D,
                [&]() { obj.cameraFollow2D = CameraFollow2DComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::CameraFollow2D; inspectorClipboard.cameraFollow2D = obj.cameraFollow2D; },
                [&]() { obj.hasCameraFollow2D = true; obj.cameraFollow2D = inspectorClipboard.cameraFollow2D; changed = true; },
                [&]() { obj.cameraFollow2D = inspectorClipboard.cameraFollow2D; changed = true; },
                removeFollow);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("CameraFollow2D");
            if (!obj.hasCamera) {
                ImGui::TextDisabled("Requires a Camera component.");
            }
            if (drawObjectReferenceSlot("##CameraFollowTarget", "Target", obj.cameraFollow2D.targetId, obj.id, "Target Object")) {
                changed = true;
            }
            if (beginCompFields("##Fields_CameraFollow2D")) {
                fieldRow(Loc::T("COMPONENT_CAMERA_FOLLOW2_D_OFFSET", "Offset"));
                if (ImGui::DragFloat2("##Offset", &obj.cameraFollow2D.offset.x, 0.1f)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_CAMERA_FOLLOW2_D_SMOOTH_TIME", "Smooth Time"));
                if (ImGui::DragFloat("##SmoothTime", &obj.cameraFollow2D.smoothTime, 0.01f, 0.0f, 10.0f, "%.2f s")) {
                    obj.cameraFollow2D.smoothTime = std::max(0.0f, obj.cameraFollow2D.smoothTime);
                    changed = true;
                }
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeFollow) {
            obj.hasCameraFollow2D = false;
            changed = true;
        }
        if (changed) {
            cameraFollowSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "post_fx" && obj.hasPostFX) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.55f, 0.6f, 1.0f));
        bool changed = false;
        bool removePostFx = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_POST_FX", "ModuVolume"), "PostFX", "post_fx", &obj.postFx.enabled, true, [&]() {
            drawStandardComponentMenu(
                "post_fx",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::PostFX,
                [&]() { obj.postFx = PostFXSettings{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::PostFX; inspectorClipboard.postFx = obj.postFx; },
                [&]() { obj.hasPostFX = true; obj.postFx = inspectorClipboard.postFx; changed = true; },
                [&]() { obj.postFx = inspectorClipboard.postFx; changed = true; },
                removePostFx);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("PostFX");
            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Volume", nullptr, true)) {
                if (beginCompFields("##Fields_PFXVolume")) {
                    if (boolRow(Loc::T("COMPONENT_POST_FX_GLOBAL_VOLUME", "Global Volume"), &obj.postFx.isGlobal)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_PRIORITY", "Priority"));
                    if (ImGui::DragFloat("##Priority", &obj.postFx.priority, 0.05f, -100.0f, 100.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_DISTORTION_POLARIZATION", "Distortion Polarization"));
                    if (ImGui::SliderFloat("##DistortionPolarization", &obj.postFx.distortionPolarization, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_BLEND_WEIGHT", "Blend Weight"));
                    if (ImGui::SliderFloat("##BlendWeight", &obj.postFx.blendWeight, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    if (!obj.postFx.isGlobal) {
                        fieldRow(Loc::T("COMPONENT_POST_FX_BLEND_RADIUS", "Blend Radius"));
                        if (ImGui::DragFloat("##BlendRadius", &obj.postFx.blendRadius, 0.1f, 0.1f, 1000.0f, "%.2f")) {
                            obj.postFx.blendRadius = std::max(0.1f, obj.postFx.blendRadius);
                            changed = true;
                        }
                        noteRow("Local volumes use this object's transform and scale as bounds.");
                    }
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("2D Scope", nullptr, true)) {
                if (beginCompFields("##Fields_PFXScope2D")) {
                    if (boolRow(Loc::T("COMPONENT_POST_FX_SCOPE2D_ENABLED", "Limit To 2D Layers"),
                                &obj.postFx.scope2DEnabled)) { changed = true; }
                    ImGui::BeginDisabled(!obj.postFx.scope2DEnabled);
                    const char* scopeModeNames[] = { "At Or Below", "Band" };
                    int scopeMode = static_cast<int>(obj.postFx.scope2DMode);
                    fieldRow(Loc::T("COMPONENT_POST_FX_SCOPE2D_MODE", "Mode"));
                    if (ImGui::Combo("##Scope2DMode", &scopeMode, scopeModeNames, IM_ARRAYSIZE(scopeModeNames))) {
                        obj.postFx.scope2DMode = static_cast<PostFX2DScopeMode>(scopeMode);
                        changed = true;
                    }
                    if (obj.postFx.scope2DMode == PostFX2DScopeMode::Band) {
                        fieldRow(Loc::T("COMPONENT_POST_FX_SCOPE2D_MIN_ORDER", "Min Sort Order"));
                        if (ImGui::DragInt("##Scope2DMinOrder", &obj.postFx.scope2DMinOrder, 0.2f, -4096, 4096)) { changed = true; }
                    }
                    fieldRow(obj.postFx.scope2DMode == PostFX2DScopeMode::Band
                                 ? Loc::T("COMPONENT_POST_FX_SCOPE2D_MAX_ORDER", "Max Sort Order")
                                 : Loc::T("COMPONENT_POST_FX_SCOPE2D_CUTOFF", "Cutoff Sort Order"));
                    if (ImGui::DragInt("##Scope2DMaxOrder", &obj.postFx.scope2DMaxOrder, 0.2f, -4096, 4096)) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("HDR & Tone Mapping", nullptr, true)) {
                if (beginCompFields("##Fields_PFXHDR")) {
                    if (boolRow(Loc::T("COMPONENT_POST_FX_HDR_ENABLED", "HDR Enabled"), &obj.postFx.hdrEnabled)) { changed = true; }
                    const char* toneMapperNames[] = { "None", "Reinhard", "ACES" };
                    int toneMapper = static_cast<int>(obj.postFx.toneMapper);
                    fieldRow(Loc::T("COMPONENT_POST_FX_TONE_MAPPER", "Tone Mapper"));
                    if (ImGui::Combo("##ToneMapper", &toneMapper, toneMapperNames, IM_ARRAYSIZE(toneMapperNames))) {
                        obj.postFx.toneMapper = static_cast<PostFXToneMapper>(toneMapper);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_POST_FX_WHITE_POINT", "White Point"));
                    if (ImGui::SliderFloat("##WhitePoint", &obj.postFx.whitePoint, 0.25f, 16.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_OUTPUT_GAMMA", "Output Gamma"));
                    if (ImGui::SliderFloat("##OutputGamma", &obj.postFx.gamma, 1.0f, 3.0f, "%.2f")) { changed = true; }
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Bloom", nullptr, true)) {
                if (beginCompFields("##Fields_PFXBloom")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledBloom", &obj.postFx.bloomEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.bloomEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_THRESHOLD", "Threshold"));
                    if (ImGui::SliderFloat("##BloomThreshold", &obj.postFx.bloomThreshold, 0.0f, 4.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_SOFT_KNEE", "Soft Knee"));
                    if (ImGui::SliderFloat("##BloomSoftKnee", &obj.postFx.bloomSoftKnee, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_INTENSITY", "Intensity"));
                    if (ImGui::SliderFloat("##BloomIntensity", &obj.postFx.bloomIntensity, 0.0f, 4.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_SPREAD", "Spread"));
                    if (ImGui::SliderFloat("##BloomSpread", &obj.postFx.bloomRadius, 0.5f, 4.5f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Color", nullptr, true)) {
                if (beginCompFields("##Fields_PFXColor")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledColor", &obj.postFx.colorAdjustEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.colorAdjustEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_EXPOSURE_EV", "Exposure (EV)"));
                    if (ImGui::SliderFloat("##Exposure", &obj.postFx.exposure, -5.0f, 5.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_CONTRAST", "Contrast"));
                    if (ImGui::SliderFloat("##Contrast", &obj.postFx.contrast, 0.0f, 2.5f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_SATURATION", "Saturation"));
                    if (ImGui::SliderFloat("##Saturation", &obj.postFx.saturation, 0.0f, 2.5f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_COLOR_FILTER", "Color Filter"));
                    if (ImGui::ColorEdit3("##ColorFilter", &obj.postFx.colorFilter.x)) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Eye Adaptation", nullptr, true)) {
                if (beginCompFields("##Fields_PFXEyeAdapt")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledEyeAdapt", &obj.postFx.eyeAdaptationEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.eyeAdaptationEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_EA_KEY", "Key Value"));
                    if (ImGui::SliderFloat("##EAKey", &obj.postFx.eyeAdaptationKeyValue, 0.01f, 1.0f, "%.3f")) { changed = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("The average luminance the eye settles on.\n"
                                          "0.18 is photographic middle grey.");
                    }
                    fieldRow(Loc::T("COMPONENT_POST_FX_EA_MIN_EV", "Min EV"));
                    if (ImGui::SliderFloat("##EAMinEv", &obj.postFx.eyeAdaptationMinEv, -8.0f, 0.0f, "%.1f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_EA_MAX_EV", "Max EV"));
                    if (ImGui::SliderFloat("##EAMaxEv", &obj.postFx.eyeAdaptationMaxEv, 0.0f, 8.0f, "%.1f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_EA_SPEED_UP", "Speed To Light"));
                    if (ImGui::SliderFloat("##EASpeedUp", &obj.postFx.eyeAdaptationSpeedUp, 0.05f, 10.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_EA_SPEED_DOWN", "Speed To Dark"));
                    if (ImGui::SliderFloat("##EASpeedDown", &obj.postFx.eyeAdaptationSpeedDown, 0.05f, 10.0f, "%.2f")) { changed = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Keep this well below Speed To Light. Real eyes go\n"
                                          "light-adapted in seconds and dark-adapted over\n"
                                          "minutes, and that asymmetry is most of the effect.");
                    }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Purkinje Shift", nullptr, true)) {
                if (beginCompFields("##Fields_PFXPurkinje")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledPurkinje", &obj.postFx.purkinjeEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Rods take over in dim light: colour drains and the\n"
                                          "image shifts blue. Applied per pixel, so a lamp stays\n"
                                          "warm while the same room's corners go blue.");
                    }
                    ImGui::BeginDisabled(!obj.postFx.purkinjeEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_PK_STRENGTH", "Strength"));
                    if (ImGui::SliderFloat("##PKStrength", &obj.postFx.purkinjeStrength, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_PK_THRESHOLD", "Threshold"));
                    if (ImGui::SliderFloat("##PKThreshold", &obj.postFx.purkinjeThreshold, 0.0f, 0.5f, "%.3f")) { changed = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Luminance below which rods start contributing.");
                    }
                    fieldRow(Loc::T("COMPONENT_POST_FX_PK_RANGE", "Range"));
                    if (ImGui::SliderFloat("##PKRange", &obj.postFx.purkinjeRange, 0.01f, 1.0f, "%.3f")) { changed = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Width of the rod/cone crossover. Narrow makes the\n"
                                          "shift snap on at a hard luminance edge.");
                    }
                    fieldRow(Loc::T("COMPONENT_POST_FX_PK_TINT", "Tint"));
                    if (ImGui::ColorEdit3("##PKTint", &obj.postFx.purkinjeTint.x)) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Motion Blur", nullptr, true)) {
                if (beginCompFields("##Fields_PFXMotionBlur")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledMotionBlur", &obj.postFx.motionBlurEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.motionBlurEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_STRENGTH", "Strength"));
                    if (ImGui::SliderFloat("##MBStrength", &obj.postFx.motionBlurStrength, 0.0f, 0.95f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_THRESHOLD_2", "Threshold"));
                    if (ImGui::SliderFloat("##MBThreshold", &obj.postFx.motionBlurThreshold, 0.0f, 0.25f, "%.3f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_CLAMP", "Clamp"));
                    if (ImGui::SliderFloat("##MBClamp", &obj.postFx.motionBlurClamp, 0.0f, 1.5f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Lens", nullptr, true)) {
                if (beginCompFields("##Fields_PFXLens")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledVignette", &obj.postFx.vignetteEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Vignette");
                    ImGui::BeginDisabled(!obj.postFx.vignetteEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_INTENSITY_2", "Intensity"));
                    if (ImGui::SliderFloat("##VigIntensity", &obj.postFx.vignetteIntensity, 0.0f, 1.5f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_SMOOTHNESS", "Smoothness"));
                    if (ImGui::SliderFloat("##VigSmoothness", &obj.postFx.vignetteSmoothness, 0.05f, 1.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledLensFlare", &obj.postFx.lensFlareEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Lens Flare");
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Ghosts, halo and streak anchored to the sun. The sun is wherever the\n"
                                          "skydome puts it, so with Environment > Sun Follows Light on, the flare\n"
                                          "tracks the directional light. It fades out on its own when something\n"
                                          "covers the sun - occlusion is read from the image, so no extra pass.");
                    }
                    ImGui::BeginDisabled(!obj.postFx.lensFlareEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_INTENSITY_3", "Intensity"));
                    if (ImGui::SliderFloat("##FlareIntensity", &obj.postFx.lensFlareIntensity, 0.0f, 3.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_THRESHOLD", "Threshold"));
                    if (ImGui::SliderFloat("##FlareThreshold", &obj.postFx.lensFlareThreshold, 0.0f, 3.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_TINT", "Tint"));
                    if (ImGui::ColorEdit3("##FlareTint", &obj.postFx.lensFlareTint.x)) { changed = true; }

                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_GHOSTS_ON", "Ghosts"));
                    if (ImGui::Checkbox("##FlareGhostsOn", &obj.postFx.lensFlareGhostsEnabled)) { changed = true; }
                    ImGui::BeginDisabled(!obj.postFx.lensFlareGhostsEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_GHOSTS", "  Count"));
                    if (ImGui::SliderInt("##FlareGhosts", &obj.postFx.lensFlareGhosts, 0, 8)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_SPACING", "  Spacing"));
                    if (ImGui::SliderFloat("##FlareSpacing", &obj.postFx.lensFlareGhostSpacing, 0.05f, 1.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();

                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_HALO_ON", "Halo"));
                    if (ImGui::Checkbox("##FlareHaloOn", &obj.postFx.lensFlareHaloEnabled)) { changed = true; }
                    ImGui::BeginDisabled(!obj.postFx.lensFlareHaloEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_HALO", "  Radius"));
                    if (ImGui::SliderFloat("##FlareHalo", &obj.postFx.lensFlareHaloRadius, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();

                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_STREAK_ON", "Streak"));
                    if (ImGui::Checkbox("##FlareStreakOn", &obj.postFx.lensFlareStreakEnabled)) { changed = true; }
                    ImGui::BeginDisabled(!obj.postFx.lensFlareStreakEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_STREAK", "  Strength"));
                    if (ImGui::SliderFloat("##FlareStreak", &obj.postFx.lensFlareStreak, 0.0f, 2.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();

                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_GLOW_ON", "Sun Glow"));
                    if (ImGui::Checkbox("##FlareGlowOn", &obj.postFx.lensFlareGlowEnabled)) { changed = true; }
                    ImGui::BeginDisabled(!obj.postFx.lensFlareGlowEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_GLOW_SIZE", "  Size"));
                    if (ImGui::SliderFloat("##FlareGlowSize", &obj.postFx.lensFlareGlowSize, 0.01f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_FLARE_GLOW_STR", "  Strength"));
                    if (ImGui::SliderFloat("##FlareGlowStr", &obj.postFx.lensFlareGlowIntensity, 0.0f, 3.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    ImGui::EndDisabled();
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledChromatic", &obj.postFx.chromaticAberrationEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Chromatic Aberr.");
                    ImGui::BeginDisabled(!obj.postFx.chromaticAberrationEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_FRINGE_AMOUNT", "Fringe Amount"));
                    if (ImGui::SliderFloat("##FringeAmt", &obj.postFx.chromaticAmount, 0.0f, 0.01f, "%.4f")) { changed = true; }
                    ImGui::EndDisabled();
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledSharpen", &obj.postFx.sharpenEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Sharpen");
                    ImGui::BeginDisabled(!obj.postFx.sharpenEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_SHARPEN_STR", "Sharpen Str."));
                    if (ImGui::SliderFloat("##SharpenStr", &obj.postFx.sharpenStrength, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Ambient Occlusion", nullptr, true)) {
                if (beginCompFields("##Fields_PFXAO")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledAO", &obj.postFx.ambientOcclusionEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.ambientOcclusionEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_AO_RADIUS", "AO Radius"));
                    if (ImGui::SliderFloat("##AORadius", &obj.postFx.aoRadius, 0.0005f, 0.01f, "%.4f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_AO_STRENGTH", "AO Strength"));
                    if (ImGui::SliderFloat("##AOStrength", &obj.postFx.aoStrength, 0.0f, 2.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Dither", nullptr, true)) {
                if (beginCompFields("##Fields_PFXDither")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledDither", &obj.postFx.ditherEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.ditherEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_INTENSITY_3", "Intensity"));
                    if (ImGui::SliderFloat("##DitherIntensity", &obj.postFx.ditherIntensity, 0.0f, 1.5f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_COLOR_BIT_DEPTH", "Color Bit Depth"));
                    if (ImGui::SliderInt("##DitherColorBits", &obj.postFx.ditherColorBits, 1, 8, "%d bits")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_DITHER_SIZE", "Dither Size"));
                    if (ImGui::SliderFloat("##DitherSize", &obj.postFx.ditherSize, 1.0f, 8.0f, "%.1f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_CONTRAST_2", "Contrast"));
                    if (ImGui::SliderFloat("##DitherContrast", &obj.postFx.ditherContrast, -1.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_OFFSET", "Offset"));
                    if (ImGui::SliderFloat("##DitherOffset", &obj.postFx.ditherOffset, -1.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_DARK_ADJUST", "Dark Adjust"));
                    if (ImGui::SliderFloat("##DitherDarkAdj", &obj.postFx.ditherDarkAdjustment, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_PIXELATION", "Pixelation"));
                    if (ImGui::SliderFloat("##DitherPixelation", &obj.postFx.ditherPixelation, 0.0f, 64.0f, "%.1f px")) { changed = true; }
                    static const char* ditherPaletteNames[] = { "Full Color", "PS1 Warm", "PS1 Cool", "Mono", "Sepia" };
                    int ditherPalette = static_cast<int>(obj.postFx.ditherPalette);
                    fieldRow(Loc::T("COMPONENT_POST_FX_PALETTE", "Palette"));
                    if (ImGui::Combo("##DitherPalette", &ditherPalette, ditherPaletteNames, IM_ARRAYSIZE(ditherPaletteNames))) {
                        obj.postFx.ditherPalette = static_cast<PostFXDitherPalette>(ditherPalette);
                        changed = true;
                    }
                    static const char* ditherPatternNames[] = { "Classic 4x4", "Bayer 8x8", "Bayer 16x16", "Checker", "Hybrid PS1" };
                    int ditherPattern = static_cast<int>(obj.postFx.ditherPattern);
                    fieldRow(Loc::T("COMPONENT_POST_FX_PATTERN", "Pattern"));
                    if (ImGui::Combo("##DitherPattern", &ditherPattern, ditherPatternNames, IM_ARRAYSIZE(ditherPatternNames))) {
                        obj.postFx.ditherPattern = static_cast<PostFXDitherPattern>(ditherPattern);
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Static", nullptr, true)) {
                if (beginCompFields("##Fields_PFXStatic")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledStatic", &obj.postFx.staticEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.staticEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_INTENSITY_4", "Intensity"));
                    if (ImGui::SliderFloat("##StaticIntensity", &obj.postFx.staticIntensity, 0.0f, 1.5f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_GRAIN_SCALE", "Grain Scale"));
                    if (ImGui::SliderFloat("##StaticGrainScale", &obj.postFx.staticGrainScale, 0.25f, 10.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_DARK_INFLUENCE", "Dark Influence"));
                    if (ImGui::SliderFloat("##StaticDarkInfluence", &obj.postFx.staticDarkAreaInfluence, 0.0f, 2.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_SPEED", "Speed"));
                    if (ImGui::SliderFloat("##StaticSpeed", &obj.postFx.staticSpeed, 0.0f, 20.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_MONOCHROME", "Monochrome"));
                    if (ImGui::Checkbox("##StaticMonochrome", &obj.postFx.staticMonochrome)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_SPARKLE", "Sparkle"));
                    if (ImGui::SliderFloat("##StaticSparkle", &obj.postFx.staticSparkle, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Static Distortion", nullptr, true)) {
                if (beginCompFields("##Fields_PFXStaticDist")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledStaticDist", &obj.postFx.staticDistortionEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.staticDistortionEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_HORIZ_JITTER", "Horiz Jitter"));
                    if (ImGui::SliderFloat("##HorizJitter", &obj.postFx.staticDistortionHorizontalJitterAmount, 0.0f, 0.05f, "%.4f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_LINE_DENSITY", "Line Density"));
                    if (ImGui::SliderFloat("##LineDensity", &obj.postFx.staticDistortionLineDensity, 1.0f, 256.0f, "%.1f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_GLITCH_FREQ", "Glitch Freq"));
                    if (ImGui::SliderFloat("##GlitchFreq", &obj.postFx.staticDistortionGlitchFrequency, 0.0f, 20.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_DIST_STRENGTH", "Dist Strength"));
                    if (ImGui::SliderFloat("##DistStrength", &obj.postFx.staticDistortionStrength, 0.0f, 1.5f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Lens Distortion", nullptr, true)) {
                if (beginCompFields("##Fields_PFXLensDist")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledLensDist", &obj.postFx.lensDistortionEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.lensDistortionEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_DIST_AMOUNT", "Dist Amount"));
                    if (ImGui::SliderFloat("##LensDistAmt", &obj.postFx.lensDistortionAmount, -1.0f, 1.0f, "%.3f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_EDGE_FALLOFF", "Edge Falloff"));
                    if (ImGui::SliderFloat("##LensEdgeFalloff", &obj.postFx.lensDistortionEdgeFalloff, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_CENTER_OFFSET", "Center Offset"));
                    if (ImGui::DragFloat2("##LensCenterOffset", &obj.postFx.lensDistortionCenterOffset.x, 0.001f, -0.25f, 0.25f, "%.3f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_EDGE_VIGNETTE", "Edge Vignette"));
                    if (ImGui::Checkbox("##LensEdgeVignetteEnabled", &obj.postFx.lensDistortionEdgeVignetteEnabled)) { changed = true; }
                    ImGui::BeginDisabled(!obj.postFx.lensDistortionEdgeVignetteEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_EDGE_INTENSITY", "Edge Intensity"));
                    if (ImGui::SliderFloat("##LensEdgeVignetteIntensity", &obj.postFx.lensDistortionEdgeVignetteIntensity, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_EDGE_RADIUS", "Edge Radius"));
                    if (ImGui::SliderFloat("##LensEdgeVignetteRadius", &obj.postFx.lensDistortionEdgeVignetteRadius, 0.0f, 1.4142f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_EDGE_SOFTNESS", "Edge Softness"));
                    if (ImGui::SliderFloat("##LensEdgeVignetteSoftness", &obj.postFx.lensDistortionEdgeVignetteSoftness, 0.001f, 1.0f, "%.3f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_EDGE_COLOR", "Edge Color"));
                    if (ImGui::ColorEdit3("##LensEdgeVignetteColor", &obj.postFx.lensDistortionEdgeVignetteColor.x, ImGuiColorEditFlags_Float)) { changed = true; }
                    ImGui::EndDisabled();
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Pixelation", nullptr, true)) {
                if (beginCompFields("##Fields_PFXPixelation")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledPixelation", &obj.postFx.pixelationEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.pixelationEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_PIXEL_SIZE", "Pixel Size"));
                    if (ImGui::SliderFloat("##PixelationSize", &obj.postFx.pixelationSize, 1.0f, 64.0f, "%.1f px")) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Posterize", nullptr, true)) {
                if (beginCompFields("##Fields_PFXPosterize")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledPosterize", &obj.postFx.posterizeEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.posterizeEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_LEVELS", "Levels"));
                    if (ImGui::SliderInt("##PosterizeLevels", &obj.postFx.posterizeLevels, 2, 64)) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Scanlines", nullptr, true)) {
                if (beginCompFields("##Fields_PFXScanlines")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledScanlines", &obj.postFx.scanlinesEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.scanlinesEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_INTENSITY_5", "Intensity"));
                    if (ImGui::SliderFloat("##ScanlinesIntensity", &obj.postFx.scanlinesIntensity, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_DENSITY", "Density"));
                    if (ImGui::SliderFloat("##ScanlinesDensity", &obj.postFx.scanlinesDensity, 0.25f, 8.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_SPEED_2", "Speed"));
                    if (ImGui::SliderFloat("##ScanlinesSpeed", &obj.postFx.scanlinesSpeed, -10.0f, 10.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("NTSC VHS", nullptr, true)) {
                if (beginCompFields("##Fields_PFXVHS")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledVHS", &obj.postFx.vhsOverlayEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.vhsOverlayEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_SIGNAL_MODE", "Signal Mode"));
                    static const char* vhsSignalModeNames[] = { "NTSC Composite", "NTSC S-Video", "NTSC RF (Antenna)", "VHS SP", "VHS LP", "VHS EP (Worn)" };
                    int vhsSignalMode = static_cast<int>(obj.postFx.vhsOverlaySignalMode);
                    if (ImGui::Combo("##VHSSignalMode", &vhsSignalMode, vhsSignalModeNames, IM_ARRAYSIZE(vhsSignalModeNames))) {
                        obj.postFx.vhsOverlaySignalMode = static_cast<PostFXVhsSignalMode>(vhsSignalMode);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_POST_FX_INTENSITY_6", "Intensity"));
                    if (ImGui::SliderFloat("##VHSOpacity", &obj.postFx.vhsOverlayOpacity, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_NOISE_AMOUNT", "Noise Amount"));
                    if (ImGui::SliderFloat("##VHSTapeNoise", &obj.postFx.vhsOverlayTapeNoise, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_SCANLINE_STR", "Scanline Str."));
                    if (ImGui::SliderFloat("##VHSScanline", &obj.postFx.vhsOverlayScanlineStrength, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_CHROMA_OFFSET", "Chroma Offset"));
                    if (ImGui::SliderFloat("##VHSChromaBleed", &obj.postFx.vhsOverlayChromaBleed, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_DISTORTION_STR", "Distortion Str."));
                    if (ImGui::SliderFloat("##VHSDistortionStrength", &obj.postFx.vhsOverlayDistortionStrength, 0.0f, 2.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_ANIM_SPEED", "Anim Speed"));
                    if (ImGui::SliderFloat("##VHSAnimationSpeed", &obj.postFx.vhsOverlayAnimationSpeed, 0.0f, 4.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_TRACKING_ERR", "Tracking Err."));
                    if (ImGui::SliderFloat("##VHSBandHeight", &obj.postFx.vhsOverlayBottomNoiseBandHeight, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_COLOR_BLEED", "Color Bleed"));
                    if (ImGui::SliderFloat("##VHSColorBleed", &obj.postFx.vhsOverlayColorBleed, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_COLOR_BANDING", "Color Banding"));
                    if (ImGui::SliderFloat("##VHSBanding", &obj.postFx.vhsOverlayBanding, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_HEAD_SWITCHING", "Head Switching"));
                    if (ImGui::SliderFloat("##VHSBandIntensity", &obj.postFx.vhsOverlayBottomNoiseBandIntensity, 0.0f, 2.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_DROPOUTS", "Dropouts"));
                    if (ImGui::SliderFloat("##VHSDropouts", &obj.postFx.vhsOverlayDropouts, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Wavy Effect", nullptr, true)) {
                if (beginCompFields("##Fields_PFXWavy")) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##EnabledWavy", &obj.postFx.wavyEnabled)) { changed = true; }
                    ImGui::TableSetColumnIndex(1); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Enabled");
                    ImGui::BeginDisabled(!obj.postFx.wavyEnabled);
                    fieldRow(Loc::T("COMPONENT_POST_FX_AMPLITUDE", "Amplitude"));
                    if (ImGui::SliderFloat("##WavyAmplitude", &obj.postFx.wavyAmplitude, 0.0f, 0.05f, "%.4f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_FREQUENCY", "Frequency"));
                    if (ImGui::SliderFloat("##WavyFrequency", &obj.postFx.wavyFrequency, 0.1f, 80.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_POST_FX_SPEED_3", "Speed"));
                    if (ImGui::SliderFloat("##WavySpeed", &obj.postFx.wavySpeed, 0.0f, 20.0f, "%.2f")) { changed = true; }
                    if (boolRow(Loc::T("COMPONENT_POST_FX_VERTICAL", "Vertical"), &obj.postFx.wavyVertical)) { changed = true; }
                    ImGui::EndDisabled();
                    endCompFields();
                }
            }

            ImGui::PopID();
        }
        if (removePostFx) {
            obj.hasPostFX = false;
            UpdateLegacyTypeFromComponents(obj);
            changed = true;
        }
        if (changed) {
            postFxSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "reflection_cast" && obj.hasReflectionCast && sharedReflectionCast) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.28f, 0.42f, 0.55f, 1.0f));
        bool changed = false;
        bool removeReflectionCast = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_REFLECTION_CAST", "Reflection Cast"), "ReflectionCast", "reflection_cast", &obj.reflectionCast.enabled, true, [&]() {
            drawStandardComponentMenu(
                "reflection_cast",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::ReflectionCast,
                [&]() { obj.reflectionCast = ReflectionCastComponent{}; changed = true; },
                [&]() { inspectorClipboard.kind = InspectorClipboardKind::ReflectionCast; inspectorClipboard.reflectionCast = obj.reflectionCast; },
                [&]() { obj.hasReflectionCast = true; obj.reflectionCast = inspectorClipboard.reflectionCast; UpdateLegacyTypeFromComponents(obj); changed = true; },
                [&]() { obj.reflectionCast = inspectorClipboard.reflectionCast; changed = true; },
                removeReflectionCast);
        });
        if (header.enabledChanged) {
            obj.reflectionCast.baked = false;
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("ReflectionCast");
            if (beginCompFields("##Fields_ReflectionCast")) {
                int updateMode = static_cast<int>(obj.reflectionCast.updateMode);
                const char* modeLabels[] = { "Every Frame", "First Frame" };
                fieldRow(Loc::T("COMPONENT_REFLECTION_CAST_REALTIME", "Realtime"));
                if (ImGui::Combo("##ReflectionCastUpdate", &updateMode, modeLabels, IM_ARRAYSIZE(modeLabels))) {
                    obj.reflectionCast.updateMode = updateMode == 0
                        ? ReflectionCastUpdateMode::EveryFrame
                        : ReflectionCastUpdateMode::FirstFrame;
                    obj.reflectionCast.baked = false;
                    changed = true;
                }
                if (obj.reflectionCast.updateMode == ReflectionCastUpdateMode::EveryFrame) {
                    const Modularity::HardwareProfile::Tier tier =
                        projectManager.preferences.effectiveTier();
                    if (tier != Modularity::HardwareProfile::Tier::High) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
                        ImGui::TextWrapped(
                            "Every Frame re-renders 6 cube faces at %dx%d per frame. This "
                            "machine measured as %s hardware and will likely struggle - "
                            "First Frame bakes once and costs nothing after that.",
                            obj.reflectionCast.resolution, obj.reflectionCast.resolution,
                            Modularity::HardwareProfile::ToString(tier));
                        ImGui::PopStyleColor();
                    }
                }
                fieldRow(Loc::T("COMPONENT_REFLECTION_CAST_BOX_SIZE", "Box Size"));
                if (ImGui::DragFloat3("##ReflectionCastBox", &obj.reflectionCast.boxSize.x, 0.05f, 0.1f, 500.0f, "%.2f")) {
                    obj.reflectionCast.boxSize = glm::max(obj.reflectionCast.boxSize, glm::vec3(0.1f));
                    obj.reflectionCast.baked = false;
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_REFLECTION_CAST_BLEND_DISTANCE", "Blend Distance"));
                if (ImGui::DragFloat("##ReflectionCastBlend", &obj.reflectionCast.blendDistance, 0.05f, 0.0f, 500.0f, "%.2f")) {
                    obj.reflectionCast.blendDistance = std::max(0.0f, obj.reflectionCast.blendDistance);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_REFLECTION_CAST_INTENSITY", "Intensity"));
                if (ImGui::SliderFloat("##ReflectionCastIntensity", &obj.reflectionCast.intensity, 0.0f, 2.0f, "%.2f")) {
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_REFLECTION_CAST_RESOLUTION", "Resolution"));
                int resolution = std::clamp(obj.reflectionCast.resolution, 32, 1024);
                if (ImGui::SliderInt("##ReflectionCastResolution", &resolution, 32, 1024)) {
                    obj.reflectionCast.resolution = resolution;
                    obj.reflectionCast.baked = false;
                    changed = true;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Button("Bake Now")) {
                    obj.reflectionCast.baked = false;
                    changed = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled(obj.reflectionCast.baked ? "Baked" : "Pending");
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeReflectionCast) {
            obj.hasReflectionCast = false;
            obj.reflectionCast = ReflectionCastComponent{};
            UpdateLegacyTypeFromComponents(obj);
            changed = true;
        }
        if (changed) {
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "renderer" && obj.hasRenderer && sharedRenderer) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        bool rendererChanged = false;
        bool removeRenderer = false;
        auto rendererHeader = drawComponentHeader(Loc::T("COMPONENT_RENDERER", "Renderer"), "Renderer", "renderer", nullptr, true, [&]() {
            drawStandardComponentMenu(
                "renderer",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Renderer,
                [&]() {
                    resetRendererComponent(obj);
                    rendererChanged = true;
                },
                [&]() {
                    copyRendererClipboard(obj);
                },
                [&]() {
                    applyRendererClipboard(obj);
                    rendererChanged = true;
                },
                [&]() {
                    applyRendererClipboard(obj);
                    rendererChanged = true;
                },
                removeRenderer);
        });
        if (rendererHeader.open) {
            InspectorBodyScope _ibs(*this);

            int& selectedMaterialSlot = selectedRendererMaterialSlots[obj.id];
            const int materialSlotCount = 1 + static_cast<int>(obj.additionalMaterialPaths.size());
            selectedMaterialSlot = std::clamp(selectedMaterialSlot, 0, std::max(0, materialSlotCount - 1));

            auto getBuiltInMeshLabel = [&]() -> const char* {
                switch (obj.renderType) {
                    case RenderType::Cube: return "Built-in Cube";
                    case RenderType::Sphere: return "Built-in Sphere";
                    case RenderType::Capsule: return "Built-in Capsule";
                    case RenderType::Plane: return "Built-in Plane";
                    case RenderType::Torus: return "Built-in Torus";
                    case RenderType::Sprite: return "Built-in Quad";
                    case RenderType::Mirror: return "Built-in Mirror Quad";
                    case RenderType::OBJMesh:
                    case RenderType::Model:
                    case RenderType::None:
                    default: return "No mesh assigned";
                }
            };

            auto assignPrimaryMaterialPath = [&](const std::string& nextPath, bool loadAsset) {
                if (obj.materialPath == nextPath) {
                    return false;
                }
                obj.materialPath = nextPath;
                if (loadAsset && !obj.materialPath.empty()) {
                    loadMaterialFromFile(obj);
                }
                return true;
            };

            auto drawMaterialSlotRow = [&](int slotIndex, std::string& pathRef, bool primarySlot) {
                bool slotChanged = false;
                ImGui::PushID(slotIndex);
                ImGui::RadioButton("##SelectedSlot", &selectedMaterialSlot, slotIndex);
                ImGui::SameLine();
                ImGui::TextDisabled("Slot %d", slotIndex);
                // Remove sits with the slot number so the reference slot itself can run to the
                // right edge like every other one.
                if (!primarySlot) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) {
                        obj.additionalMaterialPaths.erase(
                            obj.additionalMaterialPaths.begin() + static_cast<long>(slotIndex - 1));
                        selectedMaterialSlot = std::clamp(
                            selectedMaterialSlot, 0,
                            std::max(0, static_cast<int>(obj.additionalMaterialPaths.size())));
                        ImGui::PopID();
                        return true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove this material slot");
                    }
                }
                ImGui::SameLine();
                fs::path assignedMaterial;
                if (drawAssetReferenceSlot("##MaterialSlotPath", nullptr, pathRef,
                                           FileCategory::Material, "Material", assignedMaterial)) {
                    const std::string nextPath =
                        assignedMaterial.empty() ? std::string() : assignedMaterial.string();
                    if (primarySlot) {
                        slotChanged |= assignPrimaryMaterialPath(nextPath, true);
                    } else if (pathRef != nextPath) {
                        pathRef = nextPath;
                        slotChanged = true;
                    }
                }
                /*if (primarySlot && pathRef.empty()) {
                    ImGui::TextDisabled("Slot 0 uses embedded material data until a material asset is assigned.");
                }*/
                ImGui::PopID();
                return slotChanged;
            };

            ImGui::Spacing();
            ImGui::Separator();
            /*ImGui::TextDisabled("Mesh");*/
            const bool usesMeshAsset =
                obj.renderType == RenderType::OBJMesh || obj.renderType == RenderType::Model;
            bool browserHasModel = false;
            if (!fileBrowser.selectedFile.empty() && fs::exists(fileBrowser.selectedFile)) {
                std::error_code modelEc;
                fs::directory_entry modelEntry(fileBrowser.selectedFile, modelEc);
                browserHasModel = !modelEc && fileBrowser.isModelFile(modelEntry);
            }

            if (usesMeshAsset) {
                // assignRendererMeshAsset does more than set the path (submesh index, bounds),
                // so the slot reports what was picked rather than writing obj.meshPath itself.
                fs::path assignedMesh;
                if (drawAssetReferenceSlot("##RendererMeshPath", "Mesh", obj.meshPath,
                                           FileCategory::Model, "Mesh", assignedMesh)) {
                    if (assignedMesh.empty()) {
                        obj.meshPath.clear();
                        rendererChanged = true;
                    } else if (assignRendererMeshAsset(obj, assignedMesh)) {
                        rendererChanged = true;
                    }
                }
                ImGui::BeginDisabled(obj.meshPath.empty());
                if (ImGui::SmallButton("Reload")) {
                    reloadRendererMeshAsset(obj);
                    rendererChanged = true;
                }
                ImGui::EndDisabled();
                if (obj.meshSourceIndex >= 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Source mesh index: %d", obj.meshSourceIndex);
                }
            } else {
                ImGui::TextDisabled("%s", getBuiltInMeshLabel());
                ImGui::SameLine();
                ImGui::BeginDisabled(!browserHasModel);
                if (ImGui::SmallButton("Replace With Selected Mesh")) {
                    if (assignRendererMeshAsset(obj, fileBrowser.selectedFile)) {
                        rendererChanged = true;
                    }
                }
                ImGui::EndDisabled();
            }

            ImGui::Spacing();
            ImGui::Separator();
            /*
            ImGui::TextDisabled("Renderer");
            if (obj.renderType == RenderType::Sprite || obj.renderType == RenderType::Mirror) {
                if (ImGui::Checkbox(Loc::Widget("COMPONENT_REFLECTION_CAST_FACE_CAMERA", "Face Camera"), &obj.faceCamera)) {
                    rendererChanged = true;
                }
            } else {
                ImGui::TextDisabled("No additional renderer flags for this render type.");
            }
            */
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Material Slots");
            rendererChanged |= drawMaterialSlotRow(0, obj.materialPath, true);
            for (size_t slot = 0; slot < obj.additionalMaterialPaths.size(); ++slot) {
                rendererChanged |= drawMaterialSlotRow(
                    static_cast<int>(slot) + 1,
                    obj.additionalMaterialPaths[slot],
                    false
                );
            }
            if (ImGui::SmallButton("Add Material Slot")) {
                obj.additionalMaterialPaths.push_back("");
                selectedMaterialSlot = static_cast<int>(obj.additionalMaterialPaths.size());
                rendererChanged = true;
            }

        }
        if (removeRenderer) {
            obj.hasRenderer = false;
            obj.renderType = RenderType::None;
            UpdateLegacyTypeFromComponents(obj);
            rendererChanged = true;
        }
        if (rendererChanged) {
            rendererSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
    }

    if (inspectorComponentKey == "light" && obj.hasLight && sharedLight) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.5f, 0.45f, 0.2f, 1.0f));
        bool changed = false;
        bool removeLight = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_LIGHT", "Light"), "Light", "light", &obj.light.enabled, true, [&]() {
            drawStandardComponentMenu(
                "light",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Light,
                [&]() {
                    const LightType preservedType = obj.light.type;
                    obj.light = LightComponent{};
                    obj.light.type = preservedType;
                    changed = true;
                },
                [&]() {
                    inspectorClipboard.kind = InspectorClipboardKind::Light;
                    inspectorClipboard.light = obj.light;
                },
                [&]() {
                    obj.hasLight = true;
                    obj.light = inspectorClipboard.light;
                    UpdateLegacyTypeFromComponents(obj);
                    changed = true;
                },
                [&]() {
                    obj.hasLight = true;
                    obj.light = inspectorClipboard.light;
                    UpdateLegacyTypeFromComponents(obj);
                    changed = true;
                },
                removeLight);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Light");
            if (beginCompFields("##Fields_Light")) {
                int currentType = static_cast<int>(obj.light.type);
                const char* typeLabels[] = { "Directional", "Point", "Spot", "Area" };
                fieldRow(Loc::T("COMPONENT_LIGHT_TYPE", "Type"));
                if (ImGui::Combo("##LightType", &currentType, typeLabels, IM_ARRAYSIZE(typeLabels))) {
                    obj.light.type = (currentType == 0 ? LightType::Directional :
                                      currentType == 1 ? LightType::Point :
                                      currentType == 2 ? LightType::Spot : LightType::Area);
                    if (obj.light.type == LightType::Directional) {
                        obj.light.intensity = 1.0f;
                    } else if (obj.light.type == LightType::Point) {
                        obj.light.range = 12.0f;
                        obj.light.intensity = 2.0f;
                    } else if (obj.light.type == LightType::Spot) {
                        obj.light.range = 15.0f;
                        obj.light.intensity = 2.5f;
                        obj.light.innerAngle = 15.0f;
                        obj.light.outerAngle = 25.0f;
                    } else if (obj.light.type == LightType::Area) {
                        obj.light.range = 10.0f;
                        obj.light.intensity = 3.0f;
                        obj.light.size = glm::vec2(2.0f, 2.0f);
                        obj.light.edgeFade = 0.2f;
                    }
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_LIGHT_COLOR", "Color"));
                if (ImGui::ColorEdit3("##LightColor", &obj.light.color.x)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_LIGHT_INTENSITY", "Intensity"));
                if (ImGui::SliderFloat("##LightIntensity", &obj.light.intensity, 0.0f, 10.0f)) { changed = true; }
                if (obj.light.type != LightType::Directional) {
                    fieldRow(Loc::T("COMPONENT_LIGHT_RANGE", "Range"));
                    if (ImGui::SliderFloat("##LightRange", &obj.light.range, 0.0f, 50.0f)) { changed = true; }
                }
                if (obj.light.type == LightType::Spot) {
                    fieldRow(Loc::T("COMPONENT_LIGHT_INNER_ANGLE", "Inner Angle"));
                    if (ImGui::SliderFloat("##InnerAngle", &obj.light.innerAngle, 1.0f, 90.0f)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_LIGHT_OUTER_ANGLE", "Outer Angle"));
                    if (ImGui::SliderFloat("##OuterAngle", &obj.light.outerAngle, obj.light.innerAngle, 120.0f)) { changed = true; }
                }
                if (obj.light.type == LightType::Area) {
                    fieldRow(Loc::T("COMPONENT_LIGHT_SIZE", "Size"));
                    if (ImGui::DragFloat2("##AreaSize", &obj.light.size.x, 0.05f, 0.1f, 10.0f)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_LIGHT_EDGE_SOFTNESS", "Edge Softness"));
                    if (ImGui::SliderFloat("##EdgeSoftness", &obj.light.edgeFade, 0.0f, 1.0f, "%.2f")) { changed = true; }
                }
                if (boolRow(Loc::T("COMPONENT_LIGHT_CAST_SHADOWS", "Cast Shadows"), &obj.light.castShadows)) { changed = true; }
                if (obj.light.castShadows) {
                    bool useGlobalShadowResolution = (obj.light.shadowResolution <= 0);
                    if (boolRow(Loc::T("COMPONENT_LIGHT_USE_GLOBAL_RESOLUTION", "Use Global Resolution"), &useGlobalShadowResolution)) {
                        obj.light.shadowResolution = useGlobalShadowResolution ? 0 : renderer.getShadowMapResolution();
                        changed = true;
                    }
                    if (!useGlobalShadowResolution) {
                        int shadowResolution = std::clamp(obj.light.shadowResolution, 128, 8192);
                        fieldRow(Loc::T("COMPONENT_LIGHT_SHADOW_RESOLUTION", "Shadow Resolution"));
                        if (ImGui::SliderInt("##ShadowResolution", &shadowResolution, 128, 8192)) {
                            obj.light.shadowResolution = shadowResolution;
                            changed = true;
                        }
                    }
                    if (boolRow(Loc::T("COMPONENT_LIGHT_SOFT_SHADOWS", "Soft Shadows"), &obj.light.softShadows)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_LIGHT_SHADOW_BIAS", "Shadow Bias"));
                    if (ImGui::SliderFloat("##ShadowBias", &obj.light.shadowBias, 0.0001f, 0.20f, "%.4f")) { changed = true; }
                    if (obj.light.softShadows) {
                        fieldRow(Loc::T("COMPONENT_LIGHT_SHADOW_SOFTNESS", "Shadow Softness"));
                        if (ImGui::SliderFloat("##ShadowSoftness", &obj.light.shadowSoftness, 0.001f, 0.20f, "%.3f")) { changed = true; }
                    }
                }
                endCompFields();
            }
            ImGui::PopID();
        }
        if (removeLight) {
            obj.hasLight = false;
            UpdateLegacyTypeFromComponents(obj);
            changed = true;
        }
        if (changed) {
            lightSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "light2d" && has2DWorldPackage() && obj.hasLight2D && sharedLight2D) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.62f, 0.54f, 0.18f, 1.0f));
        bool changed = false;
        bool removeLight2D = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_LIGHT2_D", "Light 2D"), "Light2D", "light2d", &obj.light2D.enabled, true, [&]() {
            drawStandardComponentMenu(
                "light2d",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Light2D,
                [&]() {
                    const Light2DType preservedType = obj.light2D.type;
                    obj.light2D = Light2DComponent{};
                    obj.light2D.type = preservedType;
                    if ((preservedType == Light2DType::Freeform || preservedType == Light2DType::Sprite) &&
                        obj.light2D.shapePoints.size() < 3) {
                        obj.light2D.shapePoints = {
                            glm::vec2(-2.0f, -1.5f),
                            glm::vec2(2.0f, -1.5f),
                            glm::vec2(2.5f, 1.0f),
                            glm::vec2(0.0f, 2.5f),
                            glm::vec2(-2.5f, 1.0f)
                        };
                    }
                    lighting2DRenderer.clearPolygonCache(obj.id);
                    UpdateLegacyTypeFromComponents(obj);
                    changed = true;
                },
                [&]() {
                    inspectorClipboard.kind = InspectorClipboardKind::Light2D;
                    inspectorClipboard.light2D = obj.light2D;
                },
                [&]() {
                    obj.hasLight2D = true;
                    obj.light2D = inspectorClipboard.light2D;
                    lighting2DRenderer.clearPolygonCache(obj.id);
                    UpdateLegacyTypeFromComponents(obj);
                    changed = true;
                },
                [&]() {
                    obj.hasLight2D = true;
                    obj.light2D = inspectorClipboard.light2D;
                    lighting2DRenderer.clearPolygonCache(obj.id);
                    UpdateLegacyTypeFromComponents(obj);
                    changed = true;
                },
                removeLight2D);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("Light2D");

            auto drawLayerMaskEditor = [&](const char* label, bool& targetAllLayers, uint32_t& targetLayerMask) {
                if (ImGui::Checkbox(label, &targetAllLayers)) {
                    changed = true;
                }
                if (!targetAllLayers && ImGui::TreeNode("Target Layers")) {
                    for (int layerIndex = 0; layerIndex < 32; ++layerIndex) {
                        bool enabledLayer = Light2DLayerMaskContains(targetLayerMask, layerIndex);
                        ImGui::PushID(layerIndex);
                        if (ImGui::Checkbox(std::to_string(layerIndex).c_str(), &enabledLayer)) {
                            if (enabledLayer) {
                                targetLayerMask |= Light2DLayerBit(layerIndex);
                            } else {
                                targetLayerMask &= ~Light2DLayerBit(layerIndex);
                            }
                            changed = true;
                        }
                        ImGui::PopID();
                        if ((layerIndex % 4) != 3) {
                            ImGui::SameLine();
                        }
                    }
                    ImGui::TreePop();
                }
            };

            if (beginCompFields("##Fields_Light2D")) {
                int currentType = static_cast<int>(obj.light2D.type);
                const char* typeLabels[] = { "Point", "Spot", "Freeform", "Sprite", "Global" };
                fieldRow(Loc::T("COMPONENT_LIGHT2_D_TYPE", "Type"));
                if (ImGui::Combo("##Light2DType", &currentType, typeLabels, IM_ARRAYSIZE(typeLabels))) {
                    obj.light2D.type = static_cast<Light2DType>(std::clamp(currentType, 0, 4));
                    if (obj.light2D.type == Light2DType::Freeform && obj.light2D.shapePoints.size() < 3) {
                        obj.light2D.shapePoints = {
                            glm::vec2(-2.0f, -1.5f),
                            glm::vec2(2.0f, -1.5f),
                            glm::vec2(2.5f, 1.0f),
                            glm::vec2(0.0f, 2.5f),
                            glm::vec2(-2.5f, 1.0f)
                        };
                    }
                    lighting2DRenderer.clearPolygonCache(obj.id);
                    UpdateLegacyTypeFromComponents(obj);
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_LIGHT2_D_COLOR", "Color"));
                if (ImGui::ColorEdit4("##Light2DColor", &obj.light2D.color.x)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_LIGHT2_D_INTENSITY", "Intensity"));
                if (ImGui::SliderFloat("##Light2DIntensity", &obj.light2D.intensity, 0.0f, 16.0f, "%.2f")) { changed = true; }
                if (obj.light2D.type != Light2DType::Global) {
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_RADIUS", "Radius"));
                    if (ImGui::DragFloat("##Light2DRadius", &obj.light2D.radius, 0.05f, 0.0f, 4096.0f, "%.2f")) {
                        obj.light2D.radius = std::max(0.0f, obj.light2D.radius);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_INNER_RADIUS", "Inner Radius"));
                    if (ImGui::DragFloat("##Light2DInnerRadius", &obj.light2D.innerRadius, 0.05f, 0.0f, 4096.0f, "%.2f")) {
                        obj.light2D.innerRadius = std::max(0.0f, obj.light2D.innerRadius);
                        obj.light2D.outerRadius = std::max(obj.light2D.outerRadius, obj.light2D.innerRadius);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_OUTER_RADIUS", "Outer Radius"));
                    if (ImGui::DragFloat("##Light2DOuterRadius", &obj.light2D.outerRadius, 0.05f, obj.light2D.innerRadius, 4096.0f, "%.2f")) {
                        obj.light2D.outerRadius = std::max(obj.light2D.innerRadius, obj.light2D.outerRadius);
                        obj.light2D.radius = std::max(obj.light2D.radius, obj.light2D.outerRadius);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_FALLOFF", "Falloff"));
                    if (ImGui::SliderFloat("##Light2DFalloff", &obj.light2D.falloffStrength, 0.01f, 8.0f, "%.2f")) { changed = true; }
                }
                if (obj.light2D.type == Light2DType::Spot) {
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_INNER_ANGLE", "Inner Angle"));
                    if (ImGui::SliderFloat("##Light2DInnerAngle", &obj.light2D.innerSpotAngle, 0.0f, 360.0f, "%.1f")) {
                        obj.light2D.outerSpotAngle = std::max(obj.light2D.outerSpotAngle, obj.light2D.innerSpotAngle);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_OUTER_ANGLE", "Outer Angle"));
                    if (ImGui::SliderFloat("##Light2DOuterAngle", &obj.light2D.outerSpotAngle, obj.light2D.innerSpotAngle, 360.0f, "%.1f")) { changed = true; }
                }
                fieldRow(Loc::T("COMPONENT_LIGHT2_D_BLEND_STYLE", "Blend Style"));
                {
                    int blendStyle = std::clamp(obj.light2D.blendStyle, 0, static_cast<int>(light2DBlendStyles.size()) - 1);
                    const char* currentBlend = light2DBlendStyles[static_cast<size_t>(blendStyle)].name.c_str();
                    if (ImGui::BeginCombo("##BlendStyle", currentBlend)) {
                        for (int i = 0; i < static_cast<int>(light2DBlendStyles.size()); ++i) {
                            bool selected = (i == blendStyle);
                            if (ImGui::Selectable(light2DBlendStyles[static_cast<size_t>(i)].name.c_str(), selected)) {
                                obj.light2D.blendStyle = i;
                                changed = true;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
                const char* overlapLabels[] = { "Additive", "Max", "Alpha Blend" };
                int overlapMode = static_cast<int>(obj.light2D.overlapOperation);
                fieldRow(Loc::T("COMPONENT_LIGHT2_D_OVERLAP", "Overlap"));
                if (ImGui::Combo("##Overlap", &overlapMode, overlapLabels, IM_ARRAYSIZE(overlapLabels))) {
                    obj.light2D.overlapOperation = static_cast<Light2DOverlapOperation>(std::clamp(overlapMode, 0, 2));
                    changed = true;
                }
                fieldRow(Loc::T("COMPONENT_LIGHT2_D_LIGHT_ORDER", "Light Order"));
                if (ImGui::DragInt("##LightOrder", &obj.light2D.lightOrder, 1.0f, -4096, 4096)) { changed = true; }
                endCompFields();
            }

            drawLayerMaskEditor("Target All Layers", obj.light2D.targetAllLayers, obj.light2D.targetLayerMask);

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout(
                    "Culling", nullptr, false,
                    obj.light2D.type != Light2DType::Global)) {
                if (beginCompFields("##Fields_L2DCulling")) {
                    if (boolRow(Loc::T("COMPONENT_LIGHT2_D_CULL_OFF_SCREEN", "Cull Off-Screen"), &obj.light2D.cullWhenOffscreen)) { changed = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("On: the light stops rendering once it leaves the view (saves GPU).\n"
                                          "Off: it keeps rendering even when fully off-screen \xE2\x80\x94\n"
                                          "handy for small maps or a spawn light you never want to unload.");
                    }
                    ImGui::BeginDisabled(!obj.light2D.cullWhenOffscreen);
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_SCREEN_MARGIN", "Screen Margin"));
                    if (ImGui::DragFloat("##L2DOffscreenMargin", &obj.light2D.offscreenCullMargin, 0.05f, 0.0f, 4096.0f, "%.2f")) {
                        obj.light2D.offscreenCullMargin = std::max(0.0f, obj.light2D.offscreenCullMargin);
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Extra world-space distance past the screen edge that the light\n"
                                          "stays loaded before it culls. Raise this so the light doesn't\n"
                                          "pop out while its glow is still reaching onto the screen.");
                    }
                    ImGui::EndDisabled();
                    noteRow(obj.light2D.cullWhenOffscreen
                                ? "Culls when off-screen (plus the margin below)."
                                : "Always rendered \xE2\x80\x94 never unloads off-screen.");
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Shadows", nullptr, true)) {
                if (beginCompFields("##Fields_L2DShadows")) {
                    if (boolRow(Loc::T("COMPONENT_LIGHT2_D_CAST_SHADOWS", "Cast Shadows"), &obj.light2D.castsShadows)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_SHADOW_STRENGTH", "Shadow Strength"));
                    if (ImGui::SliderFloat("##L2DShadowStr", &obj.light2D.shadowStrength, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Volumetric")) {
                if (beginCompFields("##Fields_L2DVolumetric")) {
                    if (boolRow(Loc::T("COMPONENT_LIGHT2_D_ENABLED", "Enabled"), &obj.light2D.volumetricEnabled)) { changed = true; }
                    noteRow("Volumetric accumulation is scaffolded for a later pass.");
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Normal Maps")) {
                if (beginCompFields("##Fields_L2DNormalMaps")) {
                    const char* normalQualityLabels[] = { "Disabled", "Fast", "Accurate" };
                    int quality = static_cast<int>(obj.light2D.normalMapQuality);
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_QUALITY", "Quality"));
                    if (ImGui::Combo("##NormalQuality", &quality, normalQualityLabels, IM_ARRAYSIZE(normalQualityLabels))) {
                        obj.light2D.normalMapQuality = static_cast<Light2DNormalMapQuality>(std::clamp(quality, 0, 2));
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_DISTANCE", "Distance"));
                    if (ImGui::DragFloat("##NormalDist", &obj.light2D.normalMapDistance, 0.05f, 0.0f, 64.0f, "%.2f")) {
                        obj.light2D.normalMapDistance = std::max(0.0f, obj.light2D.normalMapDistance);
                        changed = true;
                    }
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Distance Attenuation")) {
                if (beginCompFields("##Fields_L2DDistAtten")) {
                    if (boolRow(Loc::T("COMPONENT_LIGHT2_D_USE_DIST_EXPONENT", "Use Dist Exponent"), &obj.light2D.useDistanceExponent)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_DIST_EXPONENT", "Dist Exponent"));
                    if (ImGui::SliderFloat("##DistExponent", &obj.light2D.distanceExponent, 0.1f, 8.0f, "%.2f")) { changed = true; }
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Cookie")) {
                if (beginCompFields("##Fields_L2DCookie")) {
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_TEXTURE", "Texture"));
                    {
                        char cookieBuffer[512] = {};
                        std::snprintf(cookieBuffer, sizeof(cookieBuffer), "%s", obj.light2D.cookieTexturePath.c_str());
                        if (ImGui::InputText("##CookieTex", cookieBuffer, sizeof(cookieBuffer))) {
                            obj.light2D.cookieTexturePath = cookieBuffer;
                            changed = true;
                        }
                    }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_SCALE", "Scale"));
                    if (ImGui::DragFloat2("##CookieScale", &obj.light2D.cookieScale.x, 0.01f, 0.01f, 16.0f, "%.2f")) {
                        obj.light2D.cookieScale.x = std::max(0.01f, obj.light2D.cookieScale.x);
                        obj.light2D.cookieScale.y = std::max(0.01f, obj.light2D.cookieScale.y);
                        changed = true;
                    }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_ROTATION", "Rotation"));
                    if (ImGui::DragFloat("##CookieRotation", &obj.light2D.cookieRotation, 0.5f, -360.0f, 360.0f, "%.1f")) { changed = true; }
                    endCompFields();
                }
            }

            if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Flicker")) {
                if (beginCompFields("##Fields_L2DFlicker")) {
                    if (boolRow(Loc::T("COMPONENT_LIGHT2_D_ENABLED_2", "Enabled"), &obj.light2D.flicker.enabled)) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_SPEED", "Speed"));
                    if (ImGui::SliderFloat("##FlickerSpeed", &obj.light2D.flicker.speed, 0.01f, 64.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_AMOUNT", "Amount"));
                    if (ImGui::SliderFloat("##FlickerAmount", &obj.light2D.flicker.amount, 0.0f, 1.0f, "%.2f")) { changed = true; }
                    fieldRow(Loc::T("COMPONENT_LIGHT2_D_SEED", "Seed"));
                    if (ImGui::DragFloat("##FlickerSeed", &obj.light2D.flicker.seed, 0.05f, -1000.0f, 1000.0f, "%.2f")) { changed = true; }
                    endCompFields();
                }
            }

            if (obj.light2D.type == Light2DType::Freeform || obj.light2D.type == Light2DType::Sprite) {
                if (InspectorReveal _fs = drawInspectorSubsectionFoldout("Shape", nullptr, true)) {
                    if (ImGui::Button(light2DShapeEditMode && light2DShapeEditingObjectId == obj.id
                            ? "Stop Shape Edit"
                            : "Edit Shape")) {
                        if (light2DShapeEditMode && light2DShapeEditingObjectId == obj.id) {
                            light2DShapeEditMode = false;
                            light2DShapeEditingObjectId = -1;
                            light2DShapeEditingPointIndex = -1;
                        } else {
                            light2DShapeEditMode = true;
                            light2DShapeEditingObjectId = obj.id;
                            light2DShapeEditingPointIndex = -1;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Add Point")) {
                        if (obj.light2D.shapePoints.empty()) {
                            obj.light2D.shapePoints = {
                                glm::vec2(-1.5f, -1.0f),
                                glm::vec2(1.5f, -1.0f),
                                glm::vec2(0.0f, 1.5f)
                            };
                        } else {
                            obj.light2D.shapePoints.push_back(obj.light2D.shapePoints.back() + glm::vec2(0.5f, 0.5f));
                        }
                        lighting2DRenderer.clearPolygonCache(obj.id);
                        changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(obj.light2D.shapePoints.size() <= 3);
                    if (ImGui::Button("Remove Last")) {
                        obj.light2D.shapePoints.pop_back();
                        light2DShapeEditingPointIndex = std::min(light2DShapeEditingPointIndex,
                            static_cast<int>(obj.light2D.shapePoints.size()) - 1);
                        lighting2DRenderer.clearPolygonCache(obj.id);
                        changed = true;
                    }
                    ImGui::EndDisabled();

                    if (beginCompFields("##Fields_L2DShape")) {
                        fieldRow(Loc::T("COMPONENT_LIGHT2_D_FEATHER", "Feather"));
                        if (ImGui::SliderFloat("##L2DFeather", &obj.light2D.freeformFeather, 0.0f, 4.0f, "%.2f")) { changed = true; }
                        fieldRow(Loc::T("COMPONENT_LIGHT2_D_EDGE_FALLOFF", "Edge Falloff"));
                        if (ImGui::SliderFloat("##L2DEdgeFalloff", &obj.light2D.freeformEdgeFalloff, 0.1f, 8.0f, "%.2f")) { changed = true; }
                        endCompFields();
                    }

                    const Light2DPolygonCache& cache = lighting2DRenderer.updatePolygonCache(obj.id, obj.light2D);
                    if (cache.valid) {
                        ImGui::TextDisabled("Points: %d  Triangles: %d",
                                            static_cast<int>(obj.light2D.shapePoints.size()),
                                            static_cast<int>(cache.indices.size() / 3));
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.5f, 1.0f), "%s",
                                           cache.error.empty() ? "Invalid light shape." : cache.error.c_str());
                    }
                }
            }

            ImGui::PopID();
        }
        if (removeLight2D) {
            obj.hasLight2D = false;
            if (light2DShapeEditingObjectId == obj.id) {
                light2DShapeEditMode = false;
                light2DShapeEditingObjectId = -1;
                light2DShapeEditingPointIndex = -1;
            }
            lighting2DRenderer.clearPolygonCache(obj.id);
            UpdateLegacyTypeFromComponents(obj);
            changed = true;
        }
        if (changed) {
            light2DSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    if (inspectorComponentKey == "shadow_caster2d" && obj.hasShadowCaster2D && sharedShadowCaster2D) {
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.26f, 0.36f, 0.48f, 1.0f));
        bool changed = false;
        bool removeShadowCaster2D = false;
        auto header = drawComponentHeader(Loc::T("COMPONENT_SHADOW_CASTER2_D", "Shadow Caster 2D"), "ShadowCaster2D", "shadow_caster2d", &obj.shadowCaster2D.enabled, true, [&]() {
            drawStandardComponentMenu(
                "shadow_caster2d",
                "Copy Component Values",
                "Paste Component Values as New",
                "Paste Component Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::ShadowCaster2D,
                [&]() {
                    obj.shadowCaster2D = ShadowCaster2DComponent{};
                    obj.shadowCaster2D.points = {
                        glm::vec2(-1.0f, -1.0f),
                        glm::vec2(1.0f, -1.0f),
                        glm::vec2(1.0f, 1.0f),
                        glm::vec2(-1.0f, 1.0f)
                    };
                    UpdateLegacyTypeFromComponents(obj);
                    changed = true;
                },
                [&]() {
                    inspectorClipboard.kind = InspectorClipboardKind::ShadowCaster2D;
                    inspectorClipboard.shadowCaster2D = obj.shadowCaster2D;
                },
                [&]() {
                    obj.hasShadowCaster2D = true;
                    obj.shadowCaster2D = inspectorClipboard.shadowCaster2D;
                    UpdateLegacyTypeFromComponents(obj);
                    changed = true;
                },
                [&]() {
                    obj.hasShadowCaster2D = true;
                    obj.shadowCaster2D = inspectorClipboard.shadowCaster2D;
                    UpdateLegacyTypeFromComponents(obj);
                    changed = true;
                },
                removeShadowCaster2D);
        });
        if (header.enabledChanged) {
            changed = true;
        }
        if (header.open) {
            InspectorBodyScope _ibs(*this);
            ImGui::PushID("ShadowCaster2D");

            if (beginCompFields("##Fields_ShadowCaster2D")) {
                if (boolRow(Loc::T("COMPONENT_SHADOW_CASTER2_D_SELF_SHADOW", "Self Shadow"), &obj.shadowCaster2D.castsSelfShadow)) { changed = true; }
                fieldRow(Loc::T("COMPONENT_SHADOW_CASTER2_D_STRENGTH", "Strength"));
                if (ImGui::SliderFloat("##SC2DStrength", &obj.shadowCaster2D.shadowStrength, 0.0f, 1.0f, "%.2f")) { changed = true; }
                if (boolRow(Loc::T("COMPONENT_SHADOW_CASTER2_D_TARGET_ALL_LAYERS", "Target All Layers"), &obj.shadowCaster2D.targetAllLayers)) { changed = true; }
                endCompFields();
            }
            if (!obj.shadowCaster2D.targetAllLayers && ImGui::TreeNode("Target Layers")) {
                for (int layerIndex = 0; layerIndex < 32; ++layerIndex) {
                    bool enabledLayer = Light2DLayerMaskContains(obj.shadowCaster2D.targetLayerMask, layerIndex);
                    ImGui::PushID(layerIndex);
                    if (ImGui::Checkbox(std::to_string(layerIndex).c_str(), &enabledLayer)) {
                        if (enabledLayer) {
                            obj.shadowCaster2D.targetLayerMask |= Light2DLayerBit(layerIndex);
                        } else {
                            obj.shadowCaster2D.targetLayerMask &= ~Light2DLayerBit(layerIndex);
                        }
                        changed = true;
                    }
                    ImGui::PopID();
                    if ((layerIndex % 4) != 3) {
                        ImGui::SameLine();
                    }
                }
                ImGui::TreePop();
            }

            if (ImGui::Button(light2DShapeEditMode && light2DShapeEditingObjectId == obj.id
                    ? "Stop Shape Edit"
                    : "Edit Shape")) {
                if (light2DShapeEditMode && light2DShapeEditingObjectId == obj.id) {
                    light2DShapeEditMode = false;
                    light2DShapeEditingObjectId = -1;
                    light2DShapeEditingPointIndex = -1;
                } else {
                    light2DShapeEditMode = true;
                    light2DShapeEditingObjectId = obj.id;
                    light2DShapeEditingPointIndex = -1;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Point##ShadowCaster2D")) {
                if (obj.shadowCaster2D.points.empty()) {
                    obj.shadowCaster2D.points = {
                        glm::vec2(-1.0f, -1.0f),
                        glm::vec2(1.0f, -1.0f),
                        glm::vec2(1.0f, 1.0f),
                        glm::vec2(-1.0f, 1.0f)
                    };
                } else {
                    obj.shadowCaster2D.points.push_back(obj.shadowCaster2D.points.back() + glm::vec2(0.4f, 0.4f));
                }
                changed = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(obj.shadowCaster2D.points.size() <= 3);
            if (ImGui::Button("Remove Last##ShadowCaster2D")) {
                obj.shadowCaster2D.points.pop_back();
                light2DShapeEditingPointIndex = std::min(light2DShapeEditingPointIndex,
                    static_cast<int>(obj.shadowCaster2D.points.size()) - 1);
                changed = true;
            }
            ImGui::EndDisabled();

            std::string shadowError;
            if (Light2DValidatePolygon(obj.shadowCaster2D.points, &shadowError)) {
                std::vector<unsigned int> triangles;
                Light2DTriangulatePolygon(obj.shadowCaster2D.points, triangles, nullptr);
                ImGui::TextDisabled("Points: %d  Triangles: %d",
                                    static_cast<int>(obj.shadowCaster2D.points.size()),
                                    static_cast<int>(triangles.size() / 3));
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.5f, 1.0f), "%s",
                                   shadowError.empty() ? "Invalid shadow caster shape." : shadowError.c_str());
            }

            ImGui::PopID();
        }
        if (removeShadowCaster2D) {
            obj.hasShadowCaster2D = false;
            if (light2DShapeEditingObjectId == obj.id) {
                light2DShapeEditMode = false;
                light2DShapeEditingObjectId = -1;
                light2DShapeEditingPointIndex = -1;
            }
            UpdateLegacyTypeFromComponents(obj);
            changed = true;
        }
        if (changed) {
            shadowCaster2DSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        ImGui::PopStyleColor();
    }

    auto isNativeScriptLanguage = [](ScriptLanguage language) {
        return language == ScriptLanguage::Cpp || language == ScriptLanguage::C;
    };
    auto resolveExistingScriptSourcePath = [&](const fs::path& path) -> fs::path {
        if (path.empty()) return fs::path{};
        std::error_code ec;
        if (fs::exists(path, ec) && !ec) return path;
        ec.clear();
        if (path.is_relative() && projectManager.currentProject.isLoaded) {
            fs::path candidate = projectManager.currentProject.projectPath / path;
            if (fs::exists(candidate, ec) && !ec) return candidate;
            ec.clear();
            candidate = projectManager.currentProject.projectPath / "Scripts" / path.filename();
            if (fs::exists(candidate, ec) && !ec) return candidate;
        }
        return {};
    };
    auto nativeScriptBinaryIsStale = [&](const fs::path& sourcePath, const fs::path& binaryPath) {
        if (sourcePath.empty() || binaryPath.empty()) return false;
        std::error_code ec;
        if (!fs::exists(binaryPath, ec) || ec) return false;
        const fs::file_time_type binaryTime = fs::last_write_time(binaryPath, ec);
        if (ec) return false;

        auto newerThanBinary = [&](const fs::path& path) {
            std::error_code timeEc;
            if (path.empty() || !fs::exists(path, timeEc) || timeEc) return false;
            const fs::file_time_type time = fs::last_write_time(path, timeEc);
            return !timeEc && time > binaryTime;
        };

        if (newerThanBinary(sourcePath)) return true;

        const fs::path engineRoot = fs::path(__FILE__).parent_path().parent_path().parent_path();
        const fs::path apiHeader = engineRoot / "include" / "ModuCPPScriptApi.h";
        const fs::path experimentalHeader = engineRoot / "include" / "ModuCPPExperimentalScriptApi.h";
        return newerThanBinary(apiHeader) || newerThanBinary(experimentalHeader);
    };

    const auto __scriptsSecStart = std::chrono::steady_clock::now();
    if (IsInspectorScriptComponentKey(inspectorComponentKey)) {
    const int activeInspectorScriptId = ParseInspectorScriptComponentId(inspectorComponentKey);
    for (size_t i = 0; i < obj.scripts.size(); ++i) {
        if (obj.scripts[i].inspectorId != activeInspectorScriptId) {
            continue;
        }
        ImGui::PushID(static_cast<int>(i));
        if (multiSelection && i < sharedScriptByIndex.size() && !sharedScriptByIndex[i]) {
            ImGui::PopID();
            continue;
        }
        ScriptComponent& sc = obj.scripts[i];

        std::string headerLabel = "Script";
        if (sc.language == ScriptLanguage::CSharp && !sc.managedType.empty()) {
            headerLabel = sc.managedType;
        } else if (!sc.path.empty()) {
            headerLabel = fs::path(sc.path).filename().string();
        }
        std::string scriptId = "ScriptComponent" + std::to_string(i);
        auto header = drawComponentHeader(headerLabel.c_str(), scriptId.c_str(), inspectorComponentKey, &sc.enabled, true, [&]() {
            const bool nativeScript = isNativeScriptLanguage(sc.language);
            if (ImGui::MenuItem("Reset Component Values")) {
                const int inspectorId = sc.inspectorId;
                sc = ScriptComponent{};
                sc.inspectorId = inspectorId;
                scriptsChanged = true;
            }
            if (ImGui::MenuItem("Compile Selected Script", nullptr, false, nativeScript ? !sc.path.empty() : true)) {
                if (nativeScript) {
                    compileScriptFile(sc.path);
                } else {
                    compileManagedScripts();
                }
            }
            if (ImGui::MenuItem("View Script", nullptr, false, !sc.path.empty())) {
                openScriptInEditor(sc.path);
            }
            drawClipboardMenus(
                "Copy Script Values",
                "Paste Script Values as New",
                "Paste Script Values as Value Overrides",
                inspectorClipboard.kind == InspectorClipboardKind::Script,
                [&]() {
                    inspectorClipboard.kind = InspectorClipboardKind::Script;
                    inspectorClipboard.script = sc;
                    inspectorClipboard.script.inspectorId = 0;
                    inspectorClipboard.script.lastBinaryPath.clear();
                    inspectorClipboard.script.lastBinaryVerified = false;
                    inspectorClipboard.script.activeIEnums.clear();
                },
                [&]() {
                    EnsureInspectorComponentMetadata(obj);
                    ScriptComponent pasted = inspectorClipboard.script;
                    pasted.inspectorId = std::max(1, obj.nextInspectorScriptId++);
                    pasted.lastBinaryPath.clear();
                    pasted.lastBinaryVerified = false;
                    pasted.activeIEnums.clear();
                    scriptPasteInsertAfter = static_cast<int>(i);
                    scriptPasteInsertValue = pasted;
                    scriptPasteInsertAnchorKey = inspectorComponentKey;
                    scriptsChanged = true;
                },
                [&]() {
                    const int inspectorId = sc.inspectorId;
                    ScriptComponent pasted = inspectorClipboard.script;
                    pasted.inspectorId = inspectorId;
                    pasted.lastBinaryPath.clear();
                    pasted.lastBinaryVerified = false;
                    pasted.activeIEnums.clear();
                    sc = std::move(pasted);
                    scriptsChanged = true;
                });
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                scriptToRemove = static_cast<int>(i);
            }
        }, iconScript);
        if (header.enabledChanged) {
            scriptsChanged = true;
        }

        if (scriptToRemove == static_cast<int>(i)) {
            ImGui::PopID();
            continue;
        }

        if (header.open) {
            InspectorBodyScope _ibs(*this);
            /*
            ImGui::SeparatorText("Binding");
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
            if (ImGui::BeginTable("ScriptMeta", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                auto beginMetaRow = [&](const char* label) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", label);
                    ImGui::TableSetColumnIndex(1);
                };

                const char* languageLabels[] = {"C++", "C", "C#"};
                int languageIndex = 0;
                if (sc.language == ScriptLanguage::C) {
                    languageIndex = 1;
                } else if (sc.language == ScriptLanguage::CSharp) {
                    languageIndex = 2;
                }

                beginMetaRow("Language");
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::Combo("##ScriptLanguage", &languageIndex, languageLabels, IM_ARRAYSIZE(languageLabels))) {
                    if (languageIndex == 2) {
                        sc.language = ScriptLanguage::CSharp;
                    } else if (languageIndex == 1) {
                        sc.language = ScriptLanguage::C;
                    } else {
                        sc.language = ScriptLanguage::Cpp;
                    }
                    scriptsChanged = true;
                    if (sc.language == ScriptLanguage::CSharp) {
                        std::string stem = fs::path(sc.path).stem().string();
                        if (sc.managedType.empty() || sc.managedType == stem) {
                            if (auto inferred = InferManagedTypeFromFile(sc.path)) {
                                sc.managedType = *inferred;
                            } else if (!stem.empty()) {
                                sc.managedType = stem;
                            }
                        }
                    }
                }

                beginMetaRow(sc.language == ScriptLanguage::CSharp ? "Assembly Path" : "Path");
                // A script slot does not map onto one FileCategory: native scripts take their
                // own source extensions, C# takes a built .dll or a loose .cs.
                const bool nativeScript = isNativeScriptLanguage(sc.language);
                fs::path assignedScript;
                if (drawAssetReferenceSlot(
                        "##ScriptPath", nullptr, sc.path, FileCategory::Script,
                        nativeScript ? "Script" : "Assembly", assignedScript,
                        [&](const fs::path& candidate) {
                            if (nativeScript) {
                                return isNativeScriptSourcePath(candidate);
                            }
                            std::string ext = candidate.extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            return ext == ".dll" || ext == ".cs";
                        })) {
                    sc.path = assignedScript.empty() ? std::string() : assignedScript.string();
                    sc.lastBinaryPath.clear();
                    sc.lastBinaryVerified = false;
                    scriptsChanged = true;
                    if (!sc.path.empty()) {
                        if (nativeScript) {
                            sc.language = inferNativeLanguageFromPath(sc.path);
                        } else if (sc.language == ScriptLanguage::CSharp) {
                            const std::string stem = fs::path(sc.path).stem().string();
                            if (sc.managedType.empty() || sc.managedType == stem) {
                                if (auto inferred = InferManagedTypeFromFile(sc.path)) {
                                    sc.managedType = *inferred;
                                } else if (!stem.empty()) {
                                    sc.managedType = stem;
                                }
                            }
                        }
                    }
                }

                if (sc.language == ScriptLanguage::CSharp) {
                    char typeBuf[256] = {};
                    std::snprintf(typeBuf, sizeof(typeBuf), "%s", sc.managedType.c_str());
                    beginMetaRow("Managed Type");
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::InputText("##ScriptType", typeBuf, sizeof(typeBuf))) {
                        sc.managedType = typeBuf;
                        scriptsChanged = true;
                    }
                }

                ImGui::EndTable();
            }
            ImGui::PopStyleVar();

            ImGui::SeparatorText("Preview");
            if (!sc.path.empty()) {
                ImGui::TextDisabled("%s", fs::path(sc.path).filename().string().c_str());
            } else {
                ImGui::TextDisabled("Assign a script asset to expose runtime inspector fields.");
            }
            */

            if (!sc.path.empty()) {
                ScriptContext ctx;
                ctx.engine = this;
                ctx.object = &obj;
                ctx.script = &sc;
                ScriptContextBinding contextBinding(ctx);
                const SceneObject* sceneDataBeforeInspector = sceneObjects.data();
                const size_t sceneCountBeforeInspector = sceneObjects.size();
                std::string inspectorId = "ScriptInspector##" + std::to_string(obj.id) + sc.path;
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
                ImGui::Indent(8.0f);
                if (isNativeScriptLanguage(sc.language)) {
                    fs::path binary;
                    const bool attachedBinaryDirectly = IsNativeBinaryPath(fs::path(sc.path));
                    if (attachedBinaryDirectly) {
                        binary = fs::path(sc.path);
                    } else if (!sc.lastBinaryPath.empty()) {
                        fs::path cachedBinary = sc.lastBinaryPath;
                        std::error_code __ec;
                        if (fs::exists(cachedBinary, __ec)) {
                            binary = std::move(cachedBinary);
                        }
                    }
                    if (binary.empty()) {
                        const std::string stem = NativeScriptArtifactStem(fs::path(sc.path));
                        if (!stem.empty() && projectManager.currentProject.isLoaded) {
#if defined(_WIN32)
                            const char* ext = ".dll";
#else
                            const char* ext = ".so";
#endif
                            const fs::path candidates[] = {
                                projectManager.currentProject.projectPath / "Library" / "CompiledScripts" / (stem + ext),
                                projectManager.currentProject.projectPath / "Cache" / "ScriptBin" / (stem + ext),
                            };
                            for (const auto& cand : candidates) {
                                std::error_code __ec;
                                if (fs::exists(cand, __ec)) { binary = cand; break; }
                            }
                        }
                        if (binary.empty()) {
                            binary = resolveScriptBinary(sc.path);
                        }
                    }
                    sc.lastBinaryPath = binary.string();
                    sc.lastBinaryVerified = !binary.empty();
                    fs::path sourceForStaleCheck = resolveExistingScriptSourcePath(fs::path(sc.path));
                    const bool binaryStale =
                        !attachedBinaryDirectly && nativeScriptBinaryIsStale(sourceForStaleCheck, binary);
                    if (binaryStale) {
                        sc.lastBinaryPath.clear();
                        sc.lastBinaryVerified = false;
                        bool queuedRecompile = false;
                        if (!sourceForStaleCheck.empty()) {
                            std::error_code __staleEc;
                            const fs::file_time_type srcTime =
                                fs::last_write_time(sourceForStaleCheck, __staleEc);
                            const std::string staleKey =
                                fs::absolute(sourceForStaleCheck, __staleEc).lexically_normal().string();
                            auto attemptedIt = inspectorStaleRecompileAttempted.find(staleKey);
                            const bool alreadyAttempted =
                                !__staleEc &&
                                attemptedIt != inspectorStaleRecompileAttempted.end() &&
                                attemptedIt->second == srcTime;
                            if (!alreadyAttempted) {
                                nextCompileFromAuto = true;
                                queueScriptCompile(sourceForStaleCheck);
                                nextCompileFromAuto = false;
                                if (!__staleEc) inspectorStaleRecompileAttempted[staleKey] = srcTime;
                                queuedRecompile = true;
                            }
                        }
                        if (queuedRecompile) {
                            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.38f, 1.0f),
                                               "Script inspector binary is stale; recompiling before loading it.");
                        } else {
                            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                                               "Script inspector binary is stale (last build failed). Fix the script and save to retry.");
                        }
                    } else if (ScriptRuntime::InspectorFn inspector = scriptRuntime.getInspector(binary)) {
                        ImGui::PushID(inspectorId.c_str());
                        static std::unordered_set<ScriptRuntime::InspectorFn> __moduInspectorSeen;
                        const bool __moduFirst = __moduInspectorSeen.insert(inspector).second;
                        const auto __moduInspStart = std::chrono::steady_clock::now();
                        inspector(ctx);
                        const auto __moduInspEnd = std::chrono::steady_clock::now();
                        ImGui::PopID();
                        if (ctx.object == &obj && ctx.script == &sc) ctx.SaveAutoSettings();
                        const auto __moduSaveEnd = std::chrono::steady_clock::now();
                        const double __moduInspMs = std::chrono::duration<double, std::milli>(__moduInspEnd - __moduInspStart).count();
                        const double __moduSaveMs = std::chrono::duration<double, std::milli>(__moduSaveEnd - __moduInspEnd).count();
                        if (__moduFirst || __moduInspMs > Modularity::HardwareProfile::TimingBudget(10.0) || __moduSaveMs > Modularity::HardwareProfile::TimingBudget(10.0)) {
                            std::fprintf(stderr, "[ModuTimer] inspector %s inspect=%.2f save=%.2f  %s\n",
                                         __moduFirst ? "FIRST" : "draw ",
                                         __moduInspMs, __moduSaveMs, binary.string().c_str());
                        }
                    } else if (!scriptRuntime.getLastError().empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "Inspector load failed");
                        ImGui::TextWrapped("%s", scriptRuntime.getLastError().c_str());
                        requestRecompileForScriptLoadFailure(sourceForStaleCheck,
                                                             scriptRuntime.getLastFailure());
                    } else {
                        ImGui::TextDisabled("No inspector exported (Script_OnInspector)");
                    }
                } else {
                    fs::path assembly;
                    if (!sc.lastBinaryPath.empty()) {
                        fs::path cachedAssembly = sc.lastBinaryPath;
                        if (fs::exists(cachedAssembly)) {
                            assembly = std::move(cachedAssembly);
                        }
                    }
                    if (assembly.empty()) {
                        assembly = resolveManagedAssembly(sc.path);
                    }
                    sc.lastBinaryPath = assembly.string();
                    sc.lastBinaryVerified = !assembly.empty();
                    bool hasInspector = managedRuntime.hasInspector(assembly, sc.managedType);
                    if (hasInspector) {
                        ImGui::PushID(inspectorId.c_str());
                        bool ranInspector = managedRuntime.invokeInspector(assembly, sc.managedType, ctx);
                        ImGui::PopID();
                        if (ranInspector) {
                            if (ctx.object == &obj && ctx.script == &sc) ctx.SaveAutoSettings();
                        } else if (!managedRuntime.getLastError().empty()) {
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "Inspector load failed");
                            ImGui::TextWrapped("%s", managedRuntime.getLastError().c_str());
                        }
                    } else if (!managedRuntime.getLastError().empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "Inspector load failed");
                        ImGui::TextWrapped("%s", managedRuntime.getLastError().c_str());
                    } else {
                        ImGui::TextDisabled("No inspector exported (Script_OnInspector)");
                    }
                }
                ImGui::Unindent(8.0f);
                ImGui::PopStyleVar();
                if (ctx.object != &obj || ctx.script != &sc ||
                    sceneObjects.data() != sceneDataBeforeInspector ||
                    sceneObjects.size() != sceneCountBeforeInspector) {
                    // A custom inspector may create/delete/reparent objects or
                    // attach scripts. Finish UI scopes before touching old refs.
                    _ibs.finish();
                    ImGui::PopID(); // script
                    ImGui::PopID(); // object
                    if (inspectorWrapText) ImGui::PopTextWrapPos();
                    ImGui::PopStyleVar(3);
                    ImGui::End();
                    return;
                }
            }

            constexpr bool showScriptSettings = false;
            if (showScriptSettings) {
                ImGui::TextDisabled("Settings");
                for (size_t s = 0; s < sc.settings.size(); ++s) {
                    ImGui::PushID(static_cast<int>(s));
                    char keyBuf[128] = {};
                    char valBuf[256] = {};
                    std::snprintf(keyBuf, sizeof(keyBuf), "%s", sc.settings[s].key.c_str());
                    std::snprintf(valBuf, sizeof(valBuf), "%s", sc.settings[s].value.c_str());
                    auto isBoolString = [](const std::string& v, bool& out) {
                        if (v == "1" || v == "true" || v == "True") { out = true; return true; }
                        if (v == "0" || v == "false" || v == "False") { out = false; return true; }
                        return false;
                    };
                    auto isNumberString = [](const std::string& v, float& out) {
                        if (v.empty()) return false;
                        char* end = nullptr;
                        out = std::strtof(v.c_str(), &end);
                        return end && *end == '\0';
                    };
                    bool boolVal = false;
                    bool hasBool = isBoolString(sc.settings[s].value, boolVal);
                    float numVal = 0.0f;
                    bool hasNumber = isNumberString(sc.settings[s].value, numVal);
                    ImGui::SetNextItemWidth(
                        std::min(140.0f, ImGui::GetContentRegionAvail().x * 0.34f));
                    if (ImGui::InputText("##Key", keyBuf, sizeof(keyBuf))) {
                        sc.settings[s].key = keyBuf;
                        scriptsChanged = true;
                    }
                    ImGui::SameLine();
                    const bool settingButtonsInline = fieldWidthBeforeButtons(
                        smallButtonRunWidth({"As Bool", "As Number", "X"}), 60.0f);
                    if (hasBool) {
                        if (ImGui::Checkbox("##BoolVal", &boolVal)) {
                            sc.settings[s].value = boolVal ? "1" : "0";
                            scriptsChanged = true;
                        }
                    } else if (hasNumber) {
                        if (ImGui::InputFloat("##NumVal", &numVal, 0.0f, 0.0f, "%.4f")) {
                            sc.settings[s].value = std::to_string(numVal);
                            scriptsChanged = true;
                        }
                    } else {
                        if (ImGui::InputText("##Value", valBuf, sizeof(valBuf))) {
                            sc.settings[s].value = valBuf;
                            scriptsChanged = true;
                        }
                    }
                    if (settingButtonsInline) ImGui::SameLine();
                    ImGui::BeginDisabled(hasBool);
                    if (ImGui::SmallButton("As Bool")) {
                        sc.settings[s].value = (!sc.settings[s].value.empty() && sc.settings[s].value != "0" && sc.settings[s].value != "false") ? "1" : "0";
                        scriptsChanged = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(hasNumber);
                    if (ImGui::SmallButton("As Number")) {
                        float parsed = 0.0f;
                        if (!isNumberString(sc.settings[s].value, parsed)) parsed = 0.0f;
                        sc.settings[s].value = std::to_string(parsed);
                        scriptsChanged = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) {
                        sc.settings.erase(sc.settings.begin() + static_cast<long>(s));
                        scriptsChanged = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }

                if (ImGui::SmallButton("Add Setting")) {
                    sc.settings.push_back(ScriptSetting{"", ""});
                    scriptsChanged = true;
                }
            }
        }

        ImGui::PopID();
        break;
    }
    }

    if (scriptToRemove >= 0 && scriptToRemove < static_cast<int>(obj.scripts.size())) {
        obj.scripts.erase(obj.scripts.begin() + scriptToRemove);
        scriptsChanged = true;
        if (scriptPasteInsertAfter > scriptToRemove) {
            --scriptPasteInsertAfter;
        }
    }
    if (scriptPasteInsertAfter >= 0) {
        const size_t insertAt = std::min(static_cast<size_t>(scriptPasteInsertAfter) + 1,
                                         obj.scripts.size());
        obj.scripts.insert(obj.scripts.begin() + static_cast<std::ptrdiff_t>(insertAt),
                           scriptPasteInsertValue);
        insertInspectorKeyAfter(scriptPasteInsertAnchorKey,
                                MakeInspectorScriptComponentKey(scriptPasteInsertValue.inspectorId));
        scriptsChanged = true;
    }
    {
        const double __scriptsSecMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - __scriptsSecStart).count();
        if (__scriptsSecMs > Modularity::HardwareProfile::TimingBudget(20.0)) {
            std::fprintf(stderr, "[ModuTimer]   inspectorPanel/scriptsSection %.2f ms\n", __scriptsSecMs);
        }
    }
    }

    if (obj.hasRenderer && sharedRenderer) {
        int selectedMaterialSlot = 0;
        auto selectedSlotIt = selectedRendererMaterialSlots.find(obj.id);
        if (selectedSlotIt != selectedRendererMaterialSlots.end()) {
            selectedMaterialSlot = selectedSlotIt->second;
        }
        selectedMaterialSlot = std::clamp(
            selectedMaterialSlot,
            0,
            std::max(0, static_cast<int>(obj.additionalMaterialPaths.size()))
        );

        std::string matLine = "Material: Slot " + std::to_string(selectedMaterialSlot);
        /*if (selectedMaterialSlot == 0) {
            matLine += obj.materialPath.empty()
                ? " (Embedded)"
                : " - " + fs::path(obj.materialPath).filename().string();
        } else {
            const size_t slotIndex = static_cast<size_t>(selectedMaterialSlot - 1);
            if (slotIndex < obj.additionalMaterialPaths.size() &&
                !obj.additionalMaterialPaths[slotIndex].empty()) {
                matLine += " - " + fs::path(obj.additionalMaterialPaths[slotIndex]).filename().string();
            }
        }*/

        float textWidth = ImGui::CalcTextSize(matLine.c_str()).x;
        float availWidth = ImGui::GetContentRegionAvail().x;
        float x = ImGui::GetCursorPosX() + std::max(0.0f, availWidth - textWidth);
        ImGui::SetCursorPosX(x);
        ImGui::TextDisabled("%s", matLine.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    bool componentChanged = false;
    ImGui::PushID("AddComponent");
    ImGui::BeginDisabled(multiSelection);
    if (!multiSelection) {
        ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_A,
                                   ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Tooltip);
    }
    if (ImGui::Button(Loc::T("INSPECTOR_ADD_COMPONENT", "Add Component"), ImVec2(-1, 0))) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    ImGui::EndDisabled();
    if (multiSelection) {
        ImGui::TextDisabled("Add Component is disabled for multi-selection.");
    }
    ImGui::SetNextWindowSize(ImVec2(360.0f, 420.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("AddComponentPopup")) {
        const bool usesUIOnly2DPhysics = UsesUIOnly2DPhysics(obj);
        const bool supports3DWorldComponents = !usesUIOnly2DPhysics;
        auto applyUiDefaults = [](SceneObject& target, UIElementType type) {
            target.ui.type = type;
            switch (type) {
                case UIElementType::Canvas:
                    target.ui.label = "Canvas";
                    target.ui.size = glm::vec2(600.0f, 400.0f);
                    break;
                case UIElementType::Image:
                    target.ui.label = "Image";
                    target.ui.size = glm::vec2(200.0f, 200.0f);
                    break;
                case UIElementType::Slider:
                    target.ui.label = "Slider";
                    target.ui.size = glm::vec2(240.0f, 32.0f);
                    break;
                case UIElementType::Button:
                    target.ui.label = "Button";
                    target.ui.size = glm::vec2(160.0f, 40.0f);
                    break;
                case UIElementType::Text:
                    target.ui.label = "Text";
                    target.ui.size = glm::vec2(240.0f, 32.0f);
                    break;
                case UIElementType::Sprite2D:
                    target.ui.label = "Sprite2D";
                    target.ui.size = glm::vec2(128.0f, 128.0f);
                    break;
                case UIElementType::None:
                    break;
            }
        };

        static char componentFilter[96] = "";
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        const bool componentFilterSubmitted =
            ImGui::InputTextWithHint("##ComponentFilter", Loc::T("INSPECTOR_SEARCH_COMPONENTS", "Search components..."), componentFilter,
                                     sizeof(componentFilter), ImGuiInputTextFlags_EnterReturnsTrue);

        auto trimComponentFilter = [](const std::string& value) {
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
        };

        const std::string requestedScriptName = trimComponentFilter(componentFilter);
        std::string filterLower = componentFilter;
        std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        struct ComponentEntry {
            std::string path;
            bool enabled = true;
            std::function<void()> action;
        };
        std::vector<ComponentEntry> entries;
        auto addEntry = [&](const std::string& path, bool enabled, const std::function<void()>& action) {
            entries.push_back({path, enabled, action});
        };

        addEntry("Physics/Rigidbody 3D", !obj.hasRigidbody && supports3DWorldComponents, [&]() {
            obj.hasRigidbody = true;
            obj.rigidbody = RigidbodyComponent{};
            componentChanged = true;
        });
        addEntry("Physics/Rigidbody 2D", !obj.hasRigidbody2D && usesUIOnly2DPhysics, [&]() {
            obj.hasRigidbody2D = true;
            obj.rigidbody2D = Rigidbody2DComponent{};
            componentChanged = true;
        });
        addEntry("Physics/Collider 2D", !obj.hasCollider2D && usesUIOnly2DPhysics, [&]() {
            obj.hasCollider2D = true;
            obj.collider2D = Collider2DComponent{};
            obj.collider2D.boxSize = glm::max(obj.ui.size, glm::vec2(1.0f));
            componentChanged = true;
        });
        addEntry("Physics/Parallax Layer 2D", !obj.hasParallaxLayer2D && usesUIOnly2DPhysics, [&]() {
            obj.hasParallaxLayer2D = true;
            obj.parallaxLayer2D = ParallaxLayer2DComponent{};
            componentChanged = true;
        });
        addEntry("Networking/Network Manager",
                 !obj.hasNetworkManager && CountNetworkManagers(sceneObjects) == 0, [&]() {
            obj.hasNetworkManager = true;
            obj.networkManager = NetworkManagerComponent{};
            componentChanged = true;
        });
        addEntry("Networking/Network Identity", !obj.hasNetworkIdentity, [&]() {
            obj.hasNetworkIdentity = true;
            obj.networkIdentity = NetworkIdentityComponent{};
            componentChanged = true;
        });

        addEntry("Gameplay/Player Controller", !obj.hasPlayerController, [&]() {
            obj.hasPlayerController = true;
            obj.playerController = PlayerControllerComponent{};
            obj.hasCollider = true;
            obj.collider.type = ColliderType::Capsule;
            obj.collider.boxSize = glm::vec3(obj.playerController.radius * 2.0f, obj.playerController.height, obj.playerController.radius * 2.0f);
            obj.collider.convex = true;
            obj.hasRigidbody = true;
            obj.rigidbody.enabled = true;
            obj.rigidbody.useGravity = true;
            obj.rigidbody.isKinematic = false;
            obj.rigidbody.lockRotationX = true;
            obj.rigidbody.lockRotationY = false;
            obj.rigidbody.lockRotationZ = true;
            obj.scale = glm::vec3(obj.playerController.radius * 2.0f, obj.playerController.height, obj.playerController.radius * 2.0f);
            syncLocalTransform(obj);
            componentChanged = true;
        });
        addEntry("Audio/Audio Source", !obj.hasAudioSource, [&]() {
            obj.hasAudioSource = true;
            obj.audioSource = AudioSourceComponent{};
            componentChanged = true;
        });
        addEntry("Audio/Audio FX", !obj.hasAudioFX, [&]() {
            obj.hasAudioFX = true;
            obj.audioFX = AudioFXComponent{};
            componentChanged = true;
        });
        addEntry("Rendering/Video Player", !obj.hasVideoPlayer && HasRendererComponent(obj), [&]() {
            obj.hasVideoPlayer = true;
            obj.videoPlayer = VideoPlayerComponent{};
            componentChanged = true;
        });
        addEntry("Effects/Particle System", !obj.hasParticleSystem2D, [&]() {
            obj.hasParticleSystem2D = true;
            obj.particleSystem2D = ParticleSystem2DComponent{};
            componentChanged = true;
        });
        addEntry("Audio/Reverb Zone", !obj.hasReverbZone && supports3DWorldComponents, [&]() {
            obj.hasReverbZone = true;
            obj.reverbZone = ReverbZoneComponent{};
            obj.reverbZone.boxSize = glm::max(obj.scale, glm::vec3(1.0f));
            componentChanged = true;
        });
        addEntry("XR/XR Origin", !obj.hasXROrigin && supports3DWorldComponents, [&]() {
            obj.hasXROrigin = true;
            obj.xrOrigin = XROriginComponent{};
            componentChanged = true;
        });
        addEntry("XR/XR Camera", !obj.hasXRCamera && supports3DWorldComponents, [&]() {
            obj.hasXRCamera = true;
            obj.xrCamera = XRCameraComponent{};
            if (!obj.hasCamera) {
                obj.hasCamera = true;
                obj.camera = CameraComponent{};
                obj.camera.type = SceneCameraType::Player;
            }
            componentChanged = true;
        });
        addEntry("XR/XR Controller", !obj.hasXRController && supports3DWorldComponents, [&]() {
            obj.hasXRController = true;
            obj.xrController = XRControllerComponent{};
            componentChanged = true;
        });
        addEntry("XR/XR Action-Based Controller",
                 !obj.hasXRActionBasedController && supports3DWorldComponents, [&]() {
            obj.hasXRActionBasedController = true;
            obj.xrActionBasedController = XRActionBasedControllerComponent{};
            if (obj.hasXRController) {
                obj.xrActionBasedController.hand = obj.xrController.hand;
            }
            componentChanged = true;
        });
        addEntry("XR/XR Ray Interactor", !obj.hasXRRayInteractor && supports3DWorldComponents, [&]() {
            obj.hasXRRayInteractor = true;
            obj.xrRayInteractor = XRRayInteractorComponent{};
            componentChanged = true;
        });
        addEntry("XR/XR Direct Interactor", !obj.hasXRDirectInteractor && supports3DWorldComponents, [&]() {
            obj.hasXRDirectInteractor = true;
            obj.xrDirectInteractor = XRDirectInteractorComponent{};
            componentChanged = true;
        });
        addEntry("XR/XR Grab Interactable", !obj.hasXRGrabInteractable && supports3DWorldComponents, [&]() {
            obj.hasXRGrabInteractable = true;
            obj.xrGrabInteractable = XRGrabInteractableComponent{};
            componentChanged = true;
        });
        addEntry("AI Pathfinding/GroundBakedType", !obj.hasGroundBakedType && supports3DWorldComponents, [&]() {
            obj.hasGroundBakedType = true;
            obj.groundBakedType = GroundBakedTypeComponent{};
            showAIPathfindingWindow = true;
            componentChanged = true;
        });
        addEntry("AI Pathfinding/ObsticleObject", !obj.hasObsticleObject && supports3DWorldComponents, [&]() {
            obj.hasObsticleObject = true;
            obj.obsticleObject = ObsticleObjectComponent{};
            showAIPathfindingWindow = true;
            componentChanged = true;
        });
        addEntry("AI Pathfinding/AI Agent", !obj.hasAIAgent && supports3DWorldComponents, [&]() {
            obj.hasAIAgent = true;
            obj.aiAgent = AIAgentComponent{};
            obj.aiAgent.destination = obj.position;
            showAIPathfindingWindow = true;
            componentChanged = true;
        });
        addEntry("AI Pathfinding/OffMesh Link", !obj.hasOffMeshLink && supports3DWorldComponents, [&]() {
            obj.hasOffMeshLink = true;
            obj.offMeshLink = OffMeshLinkComponent{};
            obj.offMeshLink.startPoint = obj.position;
            obj.offMeshLink.endPoint = obj.position + glm::vec3(2.0f, 0.0f, 0.0f);
            showAIPathfindingWindow = true;
            componentChanged = true;
        });
        addEntry("Animation/Animation", !obj.hasAnimation, [&]() {
            obj.hasAnimation = true;
            obj.animation = AnimationComponent{};
            showAnimationWindow = true;
            animationTargetId = obj.id;
            componentChanged = true;
        });
        addEntry("Map Maker/Map Root", !obj.hasMapRoot && supports3DWorldComponents, [&]() {
            obj.hasMapRoot = true;
            obj.mapRoot = MapRootComponent{};
            obj.mapRoot.mapId = MapMaker::GenerateId("map");
            showSectorMapWindow = true;
            componentChanged = true;
        });
        addEntry("Map Maker/Sector", !obj.hasMapSector && supports3DWorldComponents, [&]() {
            obj.hasMapSector = true;
            obj.mapSector = MapSectorComponent{};
            obj.mapSector.sectorId = MapMaker::GenerateId("sec");
            componentChanged = true;
        });
        addEntry("Map Maker/Transition", !obj.hasMapTransition && supports3DWorldComponents, [&]() {
            obj.hasMapTransition = true;
            obj.mapTransition = MapTransitionComponent{};
            obj.mapTransition.transitionId = MapMaker::GenerateId("trn");
            componentChanged = true;
        });
        addEntry("Map Maker/Portal", !obj.hasMapPortal && supports3DWorldComponents, [&]() {
            obj.hasMapPortal = true;
            obj.mapPortal = MapPortalComponent{};
            obj.mapPortal.portalId = MapMaker::GenerateId("prt");
            const SceneObject* owner = MapMaker::FindOwningSector(sceneObjects, obj);
            if (owner != nullptr) {
                obj.mapPortal.sectorId = owner->mapSector.sectorId;
            }
            componentChanged = true;
        });
        addEntry("Map Maker/Map Mesh", !obj.hasMapMesh && supports3DWorldComponents, [&]() {
            obj.hasMapMesh = true;
            obj.mapMesh = MapMeshComponent{};
            componentChanged = true;
        });
        addEntry("Rendering/Camera", !obj.hasCamera, [&]() {
            obj.hasCamera = true;
            obj.camera = CameraComponent{};
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Rendering/Camera Follow 2D", !obj.hasCameraFollow2D && obj.hasCamera, [&]() {
            obj.hasCameraFollow2D = true;
            obj.cameraFollow2D = CameraFollow2DComponent{};
            componentChanged = true;
        });
        addEntry("Rendering/ModuVolume", !obj.hasPostFX, [&]() {
            obj.hasPostFX = true;
            obj.postFx = PostFXSettings{};
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Rendering/Reflection Cast", !obj.hasReflectionCast, [&]() {
            obj.hasReflectionCast = true;
            obj.reflectionCast = ReflectionCastComponent{};
            obj.scale = obj.reflectionCast.boxSize;
            syncLocalTransform(obj);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Lights/Directional", !obj.hasLight, [&]() {
            obj.hasLight = true;
            obj.light = LightComponent{};
            obj.light.type = LightType::Directional;
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Lights/Point", !obj.hasLight, [&]() {
            obj.hasLight = true;
            obj.light = LightComponent{};
            obj.light.type = LightType::Point;
            obj.light.range = 12.0f;
            obj.light.intensity = 2.0f;
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Lights/Spot", !obj.hasLight, [&]() {
            obj.hasLight = true;
            obj.light = LightComponent{};
            obj.light.type = LightType::Spot;
            obj.light.range = 15.0f;
            obj.light.intensity = 2.5f;
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Lights/Area", !obj.hasLight, [&]() {
            obj.hasLight = true;
            obj.light = LightComponent{};
            obj.light.type = LightType::Area;
            obj.light.range = 10.0f;
            obj.light.intensity = 3.0f;
            obj.light.size = glm::vec2(2.0f, 2.0f);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        if (has2DWorldPackage()) {
            addEntry("Lights/2D Point", !obj.hasLight2D, [&]() {
                obj.hasLight2D = true;
                obj.light2D = Light2DComponent{};
                obj.light2D.type = Light2DType::Point;
                obj.light2D.radius = 5.0f;
                obj.light2D.outerRadius = 5.0f;
                UpdateLegacyTypeFromComponents(obj);
                componentChanged = true;
            });
            addEntry("Lights/2D Spot", !obj.hasLight2D, [&]() {
                obj.hasLight2D = true;
                obj.light2D = Light2DComponent{};
                obj.light2D.type = Light2DType::Spot;
                obj.light2D.radius = 7.0f;
                obj.light2D.outerRadius = 7.0f;
                obj.light2D.innerSpotAngle = 22.0f;
                obj.light2D.outerSpotAngle = 48.0f;
                UpdateLegacyTypeFromComponents(obj);
                componentChanged = true;
            });
            addEntry("Lights/2D Freeform", !obj.hasLight2D, [&]() {
                obj.hasLight2D = true;
                obj.light2D = Light2DComponent{};
                obj.light2D.type = Light2DType::Freeform;
                obj.light2D.shapePoints = {
                    glm::vec2(-2.0f, -1.5f),
                    glm::vec2(2.0f, -1.5f),
                    glm::vec2(2.5f, 1.0f),
                    glm::vec2(0.0f, 2.5f),
                    glm::vec2(-2.5f, 1.0f)
                };
                obj.light2D.radius = 4.0f;
                obj.light2D.outerRadius = 4.0f;
                UpdateLegacyTypeFromComponents(obj);
                componentChanged = true;
            });
            addEntry("Lights/2D Global", !obj.hasLight2D, [&]() {
                obj.hasLight2D = true;
                obj.light2D = Light2DComponent{};
                obj.light2D.type = Light2DType::Global;
                obj.light2D.intensity = 0.35f;
                obj.light2D.color = glm::vec4(0.45f, 0.52f, 0.72f, 1.0f);
                UpdateLegacyTypeFromComponents(obj);
                componentChanged = true;
            });
            addEntry("Lights/2D Shadow Caster", !obj.hasShadowCaster2D, [&]() {
                obj.hasShadowCaster2D = true;
                obj.shadowCaster2D = ShadowCaster2DComponent{};
                obj.shadowCaster2D.points = {
                    glm::vec2(-1.0f, -1.0f),
                    glm::vec2(1.0f, -1.0f),
                    glm::vec2(1.0f, 1.0f),
                    glm::vec2(-1.0f, 1.0f)
                };
                UpdateLegacyTypeFromComponents(obj);
                componentChanged = true;
            });
        }
        addEntry("Renderer/Cube", true, [&]() {
            obj.hasRenderer = true;
            obj.renderType = RenderType::Cube;
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Renderer/Sphere", true, [&]() {
            obj.hasRenderer = true;
            obj.renderType = RenderType::Sphere;
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Renderer/Capsule", true, [&]() {
            obj.hasRenderer = true;
            obj.renderType = RenderType::Capsule;
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Renderer/Plane", true, [&]() {
            obj.hasRenderer = true;
            obj.renderType = RenderType::Plane;
            obj.scale = glm::vec3(2.0f, 2.0f, 0.05f);
            syncLocalTransform(obj);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Renderer/Torus", true, [&]() {
            obj.hasRenderer = true;
            obj.renderType = RenderType::Torus;
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Renderer/Sprite (Quad)", true, [&]() {
            obj.hasRenderer = true;
            obj.renderType = RenderType::Sprite;
            obj.faceCamera = false;
            obj.scale = glm::vec3(1.0f, 1.0f, 0.05f);
            obj.material.ambientStrength = 1.0f;
            syncLocalTransform(obj);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("Renderer/Mirror", true, [&]() {
            obj.hasRenderer = true;
            obj.renderType = RenderType::Mirror;
            obj.useOverlay = true;
            obj.material.textureMix = 1.0f;
            obj.material.color = glm::vec3(1.0f);
            obj.scale = glm::vec3(2.0f, 2.0f, 0.05f);
            syncLocalTransform(obj);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("UI/Canvas", true, [&]() {
            obj.hasUI = true;
            applyUiDefaults(obj, UIElementType::Canvas);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("UI/Image", true, [&]() {
            obj.hasUI = true;
            applyUiDefaults(obj, UIElementType::Image);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("UI/Slider", true, [&]() {
            obj.hasUI = true;
            applyUiDefaults(obj, UIElementType::Slider);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("UI/Button", true, [&]() {
            obj.hasUI = true;
            applyUiDefaults(obj, UIElementType::Button);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        addEntry("UI/Text", true, [&]() {
            obj.hasUI = true;
            applyUiDefaults(obj, UIElementType::Text);
            UpdateLegacyTypeFromComponents(obj);
            componentChanged = true;
        });
        for (int value = static_cast<int>(UIElementType::Panel); value <= static_cast<int>(UIElementType::DockContainer); ++value) {
            const UIElementType controlType = static_cast<UIElementType>(value);
            addEntry(std::string("UI/Controls/") + UIControls::TypeName(controlType), true, [&, controlType]() {
                obj.hasUI = true;
                UIControls::ApplyCreationDefaults(obj.ui, controlType);
                UpdateLegacyTypeFromComponents(obj);
                componentChanged = true;
            });
        }
        if (has2DWorldPackage()) {
            addEntry("UI/Sprite2D", true, [&]() {
                obj.hasUI = true;
                applyUiDefaults(obj, UIElementType::Sprite2D);
                UpdateLegacyTypeFromComponents(obj);
                componentChanged = true;
            });
        }
        addEntry("Collider/Box Collider", !obj.hasCollider, [&]() {
            obj.hasCollider = true;
            obj.collider = ColliderComponent{};
            obj.collider.boxSize = glm::max(obj.scale, glm::vec3(0.01f));
            componentChanged = true;
        });
        addEntry("Collider/Sphere Collider", !obj.hasCollider, [&]() {
            obj.hasCollider = true;
            obj.collider = ColliderComponent{};
            obj.collider.type = ColliderType::Sphere;
            obj.collider.boxSize = glm::vec3(std::max(0.01f, std::max({obj.scale.x, obj.scale.y, obj.scale.z})));
            componentChanged = true;
        });
        addEntry("Collider/Capsule Collider", !obj.hasCollider, [&]() {
            obj.hasCollider = true;
            obj.collider = ColliderComponent{};
            obj.collider.type = ColliderType::Capsule;
            obj.collider.boxSize = glm::max(obj.scale, glm::vec3(0.01f));
            componentChanged = true;
        });
        addEntry("Collider/Mesh Collider (Triangle)", !obj.hasCollider, [&]() {
            obj.hasCollider = true;
            obj.collider = ColliderComponent{};
            obj.collider.type = ColliderType::Mesh;
            obj.collider.convex = false;
            componentChanged = true;
        });
        addEntry("Collider/Mesh Collider (Convex)", !obj.hasCollider, [&]() {
            obj.hasCollider = true;
            obj.collider = ColliderComponent{};
            obj.collider.type = ColliderType::ConvexMesh;
            obj.collider.convex = true;
            componentChanged = true;
        });
        /*
        addEntry("Scripting/Empty Script Component", true, [&]() {
            obj.scripts.push_back(ScriptComponent{});
            scriptsChanged = true;
            componentChanged = true;
        });
        */

        static std::vector<fs::path> cachedScriptSources;
        static std::vector<fs::path> cachedScriptBinaries;
        static std::string cachedScriptRoot;
        static double cachedScriptRefresh = 0.0;
        static std::vector<fs::file_time_type> cachedScriptRootMtimes;
        static std::vector<fs::path> cachedScriptScannedRoots;
        double now = glfwGetTime();
        std::string projectRoot = projectManager.currentProject.projectPath.string();
        // Cheap mtime poll on the previously-scanned top-level roots; a top-level add/remove/rename
        // bumps the directory's mtime. Deep nested changes are caught by a periodic safety rebuild
        // (10s) so we don't have to walk every frame.
        bool mtimeChanged = false;
        {
            std::error_code mec;
            for (size_t i = 0; i < cachedScriptScannedRoots.size(); ++i) {
                auto t = fs::last_write_time(cachedScriptScannedRoots[i], mec);
                if (mec || i >= cachedScriptRootMtimes.size() || t != cachedScriptRootMtimes[i]) {
                    mtimeChanged = true;
                    break;
                }
            }
        }
        if (cachedScriptRoot != projectRoot || mtimeChanged || now - cachedScriptRefresh > 10.0) {
            cachedScriptRoot = projectRoot;
            cachedScriptRefresh = now;
            cachedScriptSources.clear();
            cachedScriptBinaries.clear();
            cachedScriptScannedRoots.clear();
            cachedScriptRootMtimes.clear();

            fs::path outDir;
            ScriptBuildConfig config;
            std::string error;
            fs::path cfgPath = resolveScriptsConfigPath(projectManager.currentProject);
            if (scriptCompiler.loadConfig(cfgPath, config, error)) {
                outDir = config.outDir;
            }

            std::vector<fs::path> scriptRoots = {
                projectManager.currentProject.projectPath,
                config.scriptsDir
            };
            std::error_code outDirEc;
            fs::path normalizedOutDir;
            if (!outDir.empty()) {
                normalizedOutDir = fs::weakly_canonical(outDir, outDirEc);
                if (outDirEc) {
                    normalizedOutDir = outDir.lexically_normal();
                }
            }
            std::unordered_set<std::string> seenRoots;
            std::unordered_set<std::string> seenScriptFiles;
            std::error_code ec;
            for (const auto& root : scriptRoots) {
                fs::path resolvedRoot = root;
                if (resolvedRoot.empty()) {
                    continue;
                }
                if (!resolvedRoot.is_absolute()) {
                    resolvedRoot = projectManager.currentProject.projectPath / resolvedRoot;
                }
                std::error_code rootEc;
                fs::path normalizedRoot = fs::weakly_canonical(resolvedRoot, rootEc);
                if (rootEc) {
                    normalizedRoot = resolvedRoot.lexically_normal();
                }
                std::string rootKey = normalizedRoot.lexically_normal().string();
                if (!seenRoots.insert(rootKey).second) {
                    continue;
                }
                if (!fs::exists(normalizedRoot, ec) || !fs::is_directory(normalizedRoot, ec)) {
                    continue;
                }
                std::error_code tec;
                auto rootMtime = fs::last_write_time(normalizedRoot, tec);
                if (!tec) {
                    cachedScriptScannedRoots.push_back(normalizedRoot);
                    cachedScriptRootMtimes.push_back(rootMtime);
                }
                for (auto it = fs::recursive_directory_iterator(normalizedRoot, ec);
                     it != fs::recursive_directory_iterator(); ++it) {
                    if (it->is_directory()) {
                        std::error_code dirEc;
                        fs::path normalizedDir = fs::weakly_canonical(it->path(), dirEc);
                        if (dirEc) normalizedDir = it->path().lexically_normal();
                        if (isScriptScanExcludedDir(it->path().filename().string(), it.depth() == 0) ||
                            (!normalizedOutDir.empty() && normalizedDir == normalizedOutDir)) {
                            it.disable_recursion_pending();
                        }
                        continue;
                    }
                    const std::string filename = it->path().filename().string();
                    if (filename.find(".gen.cpp") != std::string::npos ||
                        filename.find(".gen.c") != std::string::npos ||
                        filename.find(".wrap.cpp") != std::string::npos) {
                        continue;
                    }
                    std::string ext = it->path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (ext == ".cpp" || ext == ".c" || ext == ".moducpp" ||
                        ext == ".mko" || ext == ".modumako" || ext == ".cs") {
                        std::error_code fileEc;
                        fs::path canonical = fs::weakly_canonical(it->path(), fileEc);
                        if (fileEc) canonical = it->path().lexically_normal();
                        if (seenScriptFiles.insert(canonical.string()).second) {
                            cachedScriptSources.push_back(it->path());
                        }
                    }
                }
            }

            if (!outDir.empty() && fs::exists(outDir, ec)) {
                std::error_code tec;
                auto outMtime = fs::last_write_time(outDir, tec);
                if (!tec) {
                    cachedScriptScannedRoots.push_back(outDir);
                    cachedScriptRootMtimes.push_back(outMtime);
                }
                for (auto it = fs::recursive_directory_iterator(outDir, ec);
                     it != fs::recursive_directory_iterator(); ++it) {
                    if (it->is_directory()) {
                        const std::string dirName = it->path().filename().string();
                        if (dirName == ".loaded" || dirName == ".staging") {
                            it.disable_recursion_pending();
                        }
                        continue;
                    }
                    std::string ext = it->path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#ifdef _WIN32
                    if (ext == ".dll") {
                        cachedScriptBinaries.push_back(it->path());
                    }
#else
                    if (ext == ".so") {
                        cachedScriptBinaries.push_back(it->path());
                    }
#endif
                }
            }
        }

        std::unordered_map<std::string, fs::path> sourceByStem;
        sourceByStem.reserve(cachedScriptSources.size());
        for (const auto& path : cachedScriptSources) {
            sourceByStem.emplace(NativeScriptArtifactStem(path), path);
        }

        // (Battle System stuff/BossManager.moducpp vs BossManager.moducpp). Count
        std::unordered_map<std::string, int> scriptFilenameCounts;
        for (const auto& path : cachedScriptSources) {
            ++scriptFilenameCounts[path.filename().string()];
        }

        for (const auto& path : cachedScriptSources) {
            const std::string scriptFilename = path.filename().string();
            std::string label = "Scripting/" + scriptFilename;
            if (scriptFilenameCounts[scriptFilename] > 1) {
                const fs::path parent = path.parent_path();
                std::string folder = parent.filename().string();
                if (folder.empty()) folder = parent.string();
                if (!folder.empty()) {
                    label += "  (" + folder + ")";
                }
            }
            addEntry(label, true, [&, path]() {
                ScriptComponent sc;
                std::string ext = path.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".cs") {
                    sc.language = ScriptLanguage::CSharp;
                } else if (ext == ".c") {
                    sc.language = ScriptLanguage::C;
                } else {
                    sc.language = ScriptLanguage::Cpp;
                }
                sc.path = path.string();
                sc.lastBinaryPath.clear();
                sc.lastBinaryVerified = false;
                if (sc.language == ScriptLanguage::CSharp) {
                    sc.managedType = InferManagedTypeFromFile(path).value_or(path.stem().string());
                }
                obj.scripts.push_back(std::move(sc));
                EnsureInspectorComponentMetadata(obj);
                scriptsChanged = true;
                componentChanged = true;

                if (ext != ".cs") {
                    std::error_code compileEc;
                    const fs::file_time_type sourceTime = fs::last_write_time(path, compileEc);
                    if (!compileEc) {
                        queueAutoCompile(path, sourceTime);
                        processAutoCompileQueue();
                    }
                }
            });
        }

        for (const auto& bin : cachedScriptBinaries) {
            std::string stem = bin.stem().string();
            if (sourceByStem.find(stem) != sourceByStem.end()) {
                continue;
            }
            std::string label = "Scripting Compiled/" + stem;
            addEntry(label, true, [&, bin]() {
                ScriptComponent sc;
                sc.language = ScriptLanguage::Cpp;
                sc.path = bin.string();
                sc.lastBinaryPath = bin.string();
                sc.lastBinaryVerified = true;
                obj.scripts.push_back(std::move(sc));
                scriptsChanged = true;
                componentChanged = true;
            });
        }

        auto toLower = [](const std::string& value) {
            std::string lowered = value;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lowered;
        };
        auto splitPath = [](const std::string& path) {
            size_t slash = path.find('/');
            if (slash == std::string::npos) {
                return std::pair<std::string, std::string>("Misc", path);
            }
            return std::pair<std::string, std::string>(path.substr(0, slash), path.substr(slash + 1));
        };

        auto localizedCategory = [](const std::string& category) {
            const std::string key = "COMPONENT_CATEGORY_" + InspectorLocKey(category);
            return Loc::T(key.c_str(), category.c_str());
        };
        auto localizedComponentName = [](const std::string& path, const std::string& name) {
            const std::string key = "COMPONENT_MENU_" + InspectorLocKey(path);
            return Loc::T(key.c_str(), name.c_str());
        };
        auto localizedComponentPath = [&](const std::string& path) {
            const auto split = splitPath(path);
            return std::string(localizedCategory(split.first)) + "/" +
                   localizedComponentName(path, split.second);
        };

        std::vector<const ComponentEntry*> filteredEntries;
        filteredEntries.reserve(entries.size());
        for (const auto& entry : entries) {
            if (!filterLower.empty()) {
                const std::string loweredPath = toLower(entry.path);
                const std::string loweredLocalized = toLower(localizedComponentPath(entry.path));
                if (loweredPath.find(filterLower) == std::string::npos &&
                    loweredLocalized.find(filterLower) == std::string::npos) {
                    continue;
                }
            }
            filteredEntries.push_back(&entry);
        }

        if (componentFilterSubmitted && !filterLower.empty()) {
            for (const ComponentEntry* entry : filteredEntries) {
                if (!entry->enabled) {
                    continue;
                }
                entry->action();
                ImGui::CloseCurrentPopup();
                break;
            }
        }

        std::vector<std::string> categoryOrder;
        std::unordered_map<std::string, std::vector<const ComponentEntry*>> categorizedEntries;
        categorizedEntries.reserve(filteredEntries.size());
        for (const ComponentEntry* entry : filteredEntries) {
            auto split = splitPath(entry->path);
            auto itCat = categorizedEntries.find(split.first);
            if (itCat == categorizedEntries.end()) {
                categoryOrder.push_back(split.first);
            }
            categorizedEntries[split.first].push_back(entry);
        }

        auto createAndAttachScript = [&](ScriptScaffoldKind kind) {
            fs::path createdPath;
            ScriptLanguage language = ScriptLanguage::Cpp;
            std::string managedType;
            std::string createError;
            if (!createScriptAsset(kind, requestedScriptName, {}, createdPath, language, managedType, createError)) {
                addConsoleMessage("Create script failed: " + createError, ConsoleMessageType::Error);
                return;
            }

            ScriptComponent sc;
            sc.language = language;
            sc.path = createdPath.string();
            sc.lastBinaryPath.clear();
            sc.lastBinaryVerified = false;
            if (language == ScriptLanguage::CSharp) {
                sc.managedType = !managedType.empty()
                    ? managedType
                    : InferManagedTypeFromFile(createdPath).value_or(createdPath.stem().string());
            }

            obj.scripts.push_back(std::move(sc));
            EnsureInspectorComponentMetadata(obj);
            scriptsChanged = true;
            componentChanged = true;

            if (language != ScriptLanguage::CSharp) {
                std::error_code compileEc;
                const fs::file_time_type sourceTime = fs::last_write_time(createdPath, compileEc);
                if (!compileEc) {
                    queueAutoCompile(createdPath, sourceTime);
                    processAutoCompileQueue();
                }
            }
            cachedScriptSources.clear();
            cachedScriptBinaries.clear();
            cachedScriptRoot.clear();
            cachedScriptRefresh = 0.0;
            addConsoleMessage("Created and attached script: " + createdPath.string(), ConsoleMessageType::Success);
            ImGui::CloseCurrentPopup();
            openScriptInEditor(createdPath);
        };

        auto componentIconForPath = [&](const std::string& path) -> InspectorUiIcon {
            if (path.rfind("Networking/", 0) == 0) return iconCompNetwork;
            if (path.rfind("Lights/", 0) == 0) return iconCompLight;
            if (path.rfind("Audio/", 0) == 0) return iconCompAudio;
            if (path.rfind("Renderer/", 0) == 0) return iconCompMesh;
            if (path.rfind("Scripting", 0) == 0) return iconCompScript;
            if (path.rfind("Collider/", 0) == 0) return iconCompCollider;
            if (path == "Physics/Rigidbody 3D" || path == "Physics/Rigidbody 2D") return iconCompRigidbody;
            if (path == "Physics/Collider 2D") return iconCompCollider;
            if (path == "Effects/Particle System") return iconCompParticle;
            if (path == "Rendering/Video Player") return iconCompVideo;
            if (path == "Rendering/Camera" || path == "Rendering/Camera Follow 2D") return iconCompCamera;
            if (path == "Rendering/ModuVolume") return iconCompVolume;
            if (path == "UI/Text") return iconCompText;
            if (path == "UI/Image" || path == "UI/Sprite2D") return iconCompSprite;
            if (path.rfind("UI/", 0) == 0) return iconCompCanvas;
            return {};
        };
        auto drawComponentMenuIcon = [&](const InspectorUiIcon& icon) {
            const float iconSize = ImGui::GetFontSize();
            if (icon.id != static_cast<ImTextureID>(0)) {
                const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
                const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
                ImGui::Image(icon.id, ImVec2(iconSize, iconSize), uvMin, uvMax);
            } else {
                ImGui::Dummy(ImVec2(iconSize, iconSize));
            }
            ImGui::SameLine();
        };

        ImGui::Spacing();
        ImGui::TextDisabled("%s", filterLower.empty() ? Loc::T("INSPECTOR_BROWSE_CATEGORIES", "Browse categories") : Loc::T("INSPECTOR_SEARCH_RESULTS", "Search results"));
        ImVec2 listSize(ImGui::GetContentRegionAvail().x, 260.0f);
        if (ImGui::BeginChild("ComponentList", listSize, true)) {
            if (filteredEntries.empty()) {
                if (!requestedScriptName.empty()) {
                    ImGui::TextWrapped("No components matched \"%s\".", requestedScriptName.c_str());
                    ImGui::Spacing();
                    ImGui::TextDisabled("Create and attach a script instead:");
                    const float spacing = ImGui::GetStyle().ItemSpacing.x;
                    const float buttonWidth = std::max(110.0f, (ImGui::GetContentRegionAvail().x - spacing) * 0.5f);
                    if (ImGui::Button("ModuCPP", ImVec2(buttonWidth, 0.0f))) {
                        createAndAttachScript(ScriptScaffoldKind::ModuCpp);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("C++", ImVec2(buttonWidth, 0.0f))) {
                        createAndAttachScript(ScriptScaffoldKind::Cpp);
                    }
                    if (ImGui::Button("C", ImVec2(buttonWidth, 0.0f))) {
                        createAndAttachScript(ScriptScaffoldKind::C);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("ModuMAKO", ImVec2(buttonWidth, 0.0f))) {
                        createAndAttachScript(ScriptScaffoldKind::ModuMako);
                    }
                } else {
                    ImGui::TextDisabled("%s", Loc::T("INSPECTOR_NO_COMPONENTS_MATCH", "No components match the filter."));
                }
            } else if (!filterLower.empty()) {
                int filteredRowId = 0;
                for (const ComponentEntry* entry : filteredEntries) {
                    ImGui::PushID(filteredRowId++);
                    if (!entry->enabled) {
                        ImGui::BeginDisabled();
                    }
                    drawComponentMenuIcon(componentIconForPath(entry->path));
                    if (ImGui::Selectable(localizedComponentPath(entry->path).c_str())) {
                        entry->action();
                        ImGui::CloseCurrentPopup();
                    }
                    if (!entry->enabled) {
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                }
            } else {
                for (const auto& category : categoryOrder) {
                    auto itCat = categorizedEntries.find(category);
                    if (itCat == categorizedEntries.end()) continue;
                    if (ImGui::BeginMenu(localizedCategory(category))) {
                        int menuRowId = 0;
                        for (const ComponentEntry* entry : itCat->second) {
                            ImGui::PushID(menuRowId++);
                            auto split = splitPath(entry->path);
                            if (!entry->enabled) {
                                ImGui::BeginDisabled();
                            }
                            drawComponentMenuIcon(componentIconForPath(entry->path));
                            if (ImGui::MenuItem(localizedComponentName(entry->path, split.second))) {
                                entry->action();
                                ImGui::CloseCurrentPopup();
                            }
                            if (!entry->enabled) {
                                ImGui::EndDisabled();
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndMenu();
                    }
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();

    if (obj.hasRenderer && sharedRenderer) {
        ImGui::Spacing();
        const int slotCount = 1 + static_cast<int>(obj.additionalMaterialPaths.size());
        int selectedMaterialSlot = selectedRendererMaterialSlots[obj.id];
        selectedMaterialSlot = std::clamp(selectedMaterialSlot, 0, std::max(0, slotCount - 1));
        selectedRendererMaterialSlots[obj.id] = selectedMaterialSlot;
        const bool editingPrimaryMaterial = (selectedMaterialSlot == 0);
        const size_t selectedAdditionalSlotIndex = editingPrimaryMaterial
            ? 0
            : static_cast<size_t>(selectedMaterialSlot - 1);

        if (!editingPrimaryMaterial && selectedAdditionalSlotIndex < obj.additionalMaterialPaths.size()) {
            const std::string& slotPath = obj.additionalMaterialPaths[selectedAdditionalSlotIndex];
            if (slotPath != slotMaterialInspectorPath) {
                slotMaterialInspectorValid = loadMaterialData(
                    slotPath,
                    slotInspectedMaterial,
                    slotInspectedAlbedo,
                    slotInspectedOverlay,
                    slotInspectedNormal,
                    slotInspectedUseOverlay,
                    &slotInspectedShaderPack,
                    &slotInspectedVertShader,
                    &slotInspectedFragShader,
                    &slotInspectedMetallicRoughness,
                    &slotInspectedMetallicMap,
                    &slotInspectedRoughnessMap,
                    &slotInspectedOpacityMap,
                    &slotInspectedEmissive,
                    &slotInspectedOcclusion,
                    &slotInspectedHeight
                );
                slotMaterialInspectorPath = slotPath;
            }
        }

        // The selected extra slot's material, gathered back into file form so it can be saved and
        // pushed to objects that use the same .mat as their primary material.
        auto slotMaterialFileData = [&]() {
            MaterialFileData data;
            data.props = slotInspectedMaterial;
            data.albedo = slotInspectedAlbedo;
            data.overlay = slotInspectedOverlay;
            data.normal = slotInspectedNormal;
            data.metallicRoughness = slotInspectedMetallicRoughness;
            data.metallicMap = slotInspectedMetallicMap;
            data.roughnessMap = slotInspectedRoughnessMap;
            data.opacityMap = slotInspectedOpacityMap;
            data.emissive = slotInspectedEmissive;
            data.occlusion = slotInspectedOcclusion;
            data.height = slotInspectedHeight;
            data.useOverlay = slotInspectedUseOverlay;
            data.shaderPack = slotInspectedShaderPack;
            data.vertexShader = slotInspectedVertShader;
            data.fragmentShader = slotInspectedFragShader;
            return data;
        };
        auto saveSlotMaterial = [&](const std::string& slotPath) {
            const std::string filePath = resolveMaterialAssetPath(slotPath).string();
            if (!writeMaterialFile(slotMaterialFileData(), filePath)) {
                addConsoleMessage("Failed to save material: " + filePath, ConsoleMessageType::Error);
                return false;
            }
            return true;
        };
        auto reloadSlotMaterial = [&](const std::string& slotPath) {
            slotMaterialInspectorValid = loadMaterialData(
                slotPath,
                slotInspectedMaterial,
                slotInspectedAlbedo,
                slotInspectedOverlay,
                slotInspectedNormal,
                slotInspectedUseOverlay,
                &slotInspectedShaderPack,
                &slotInspectedVertShader,
                &slotInspectedFragShader,
                &slotInspectedMetallicRoughness,
                &slotInspectedMetallicMap,
                &slotInspectedRoughnessMap,
                &slotInspectedOpacityMap,
                &slotInspectedEmissive,
                &slotInspectedOcclusion,
                &slotInspectedHeight
            );
            slotMaterialInspectorPath = slotPath;
            if (slotMaterialInspectorValid) {
                propagateMaterialAsset(slotPath, slotMaterialFileData());
            }
        };
        const std::string* menuSlotPath =
            (!editingPrimaryMaterial && selectedAdditionalSlotIndex < obj.additionalMaterialPaths.size() &&
             !obj.additionalMaterialPaths[selectedAdditionalSlotIndex].empty())
                ? &obj.additionalMaterialPaths[selectedAdditionalSlotIndex]
                : nullptr;

        // Right-click menu on the Material header. Edits already save themselves and reach
        // every object sharing the .mat, so reloading/saving by hand is rare enough to live
        // here instead of as a permanent row of buttons (and a greyed Reload Shader).
        auto materialHeaderMenu = [&]() {
            const std::string* assetPath = editingPrimaryMaterial
                ? (obj.materialPath.empty() ? nullptr : &obj.materialPath)
                : menuSlotPath;
            if (assetPath) {
                const int users = countMaterialAssetUsers(*assetPath);
                if (users > 1) {
                    ImGui::TextDisabled("Shared by %d objects", users);
                } else {
                    ImGui::TextDisabled("Material asset");
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Reload Material", nullptr, false, assetPath != nullptr)) {
                if (editingPrimaryMaterial) {
                    loadMaterialFromFile(obj);
                    propagateMaterialAsset(obj.materialPath, materialFileDataFromObject(obj), obj.id);
                    rendererSectionChanged = true;
                } else {
                    reloadSlotMaterial(*menuSlotPath);
                }
            }
            if (ImGui::MenuItem("Save Material", nullptr, false,
                                assetPath != nullptr && (editingPrimaryMaterial || slotMaterialInspectorValid))) {
                if (editingPrimaryMaterial) {
                    saveMaterialToFile(obj);
                } else if (saveSlotMaterial(*menuSlotPath)) {
                    addConsoleMessage("Saved material: " + *menuSlotPath, ConsoleMessageType::Success);
                }
            }
            const std::string& menuVert = editingPrimaryMaterial ? obj.vertexShaderPath : slotInspectedVertShader;
            const std::string& menuFrag = editingPrimaryMaterial ? obj.fragmentShaderPath : slotInspectedFragShader;
            if (ImGui::MenuItem("Reload Shader", nullptr, false, !menuVert.empty() || !menuFrag.empty())) {
                renderer.forceReloadShader(menuVert, menuFrag);
            }
        };

        std::string materialHeaderName = "Material";
        const MaterialProperties* headerMaterial = &obj.material;
        const std::string* headerAlbedoPath = &obj.albedoTexturePath;
        const std::string* headerOverlayPath = &obj.overlayTexturePath;
        const std::string* headerNormalPath = &obj.normalMapPath;
        bool headerUseOverlay = obj.useOverlay;

        if (editingPrimaryMaterial) {
            if (!obj.materialPath.empty()) {
                materialHeaderName = assetDisplayName(obj.materialPath, "Material");
            }
        } else if (selectedAdditionalSlotIndex < obj.additionalMaterialPaths.size()) {
            const std::string& slotPath = obj.additionalMaterialPaths[selectedAdditionalSlotIndex];
            if (!slotPath.empty()) {
                materialHeaderName = assetDisplayName(slotPath, "Material");
            }
            if (slotMaterialInspectorValid && slotMaterialInspectorPath == slotPath) {
                headerMaterial = &slotInspectedMaterial;
                headerAlbedoPath = &slotInspectedAlbedo;
                headerOverlayPath = &slotInspectedOverlay;
                headerNormalPath = &slotInspectedNormal;
                headerUseOverlay = slotInspectedUseOverlay;
            }
        }

        auto materialHeaderState = drawMaterialSectionHeader("RendererMaterialSection",
                                                             materialHeaderName,
                                                             editingPrimaryMaterial ? obj.shaderPackPath : slotInspectedShaderPack,
                                                             editingPrimaryMaterial ? obj.vertexShaderPath : slotInspectedVertShader,
                                                             editingPrimaryMaterial ? obj.fragmentShaderPath : slotInspectedFragShader,
                                                             *headerMaterial,
                                                             *headerAlbedoPath,
                                                             *headerOverlayPath,
                                                             *headerNormalPath,
                                                             headerUseOverlay,
                                                             editingPrimaryMaterial || slotMaterialInspectorValid,
                                                             1100 + selectedMaterialSlot,
                                                             materialHeaderMenu);
        if (materialHeaderState.second) {
            rendererSectionChanged = true;
            projectManager.currentProject.hasUnsavedChanges = true;
            if (!editingPrimaryMaterial) {
                slotMaterialInspectorValid = true;
            }
        }

        if (materialHeaderState.first) {
            InspectorBodyScope _ibs(*this);
            if (selectedMaterialSlot == 0) {
                const bool primaryMaterialChanged = renderMaterialEditorBody(
                    "ObjectMaterial",
                    obj.material,
                    obj.albedoTexturePath,
                    obj.overlayTexturePath,
                    obj.normalMapPath,
                    obj.metallicRoughnessMapPath,
                    obj.metallicMapPath,
                    obj.roughnessMapPath,
                    obj.opacityMapPath,
                    obj.emissiveMapPath,
                    obj.occlusionMapPath,
                    obj.heightMapPath,
                    obj.useOverlay,
                    obj.shaderPackPath,
                    obj.vertexShaderPath,
                    obj.fragmentShaderPath,
                    &obj,
                    objectMaterialPreviewScale,
                    1002
                );

                // Reload / Save / Reload Shader and the shared-by count are in the Material
                // header's right-click menu.
                const bool hasMaterialAsset = !obj.materialPath.empty();

                // The object's material section edits the asset it names, so the change goes to
                // every object using that asset and to the file once the edit settles.
                static int pendingObjectMaterialSaveId = -1;
                if (primaryMaterialChanged) {
                    rendererSectionChanged = true;
                    projectManager.currentProject.hasUnsavedChanges = true;
                    if (hasMaterialAsset) {
                        propagateMaterialAsset(obj.materialPath, materialFileDataFromObject(obj), obj.id);
                        pendingObjectMaterialSaveId = obj.id;
                    }
                }
                if (pendingObjectMaterialSaveId == obj.id && !ImGui::IsAnyItemActive()) {
                    pendingObjectMaterialSaveId = -1;
                    if (hasMaterialAsset) {
                        // Scenes store this path project-relative; resolve it, or the write
                        // lands against the editor's working directory and fails.
                        const std::string filePath = resolveMaterialAssetPath(obj.materialPath).string();
                        if (!writeMaterialFile(materialFileDataFromObject(obj), filePath)) {
                            addConsoleMessage("Failed to save material: " + filePath, ConsoleMessageType::Error);
                        }
                    }
                }
            } else {
                const size_t slotIndex = static_cast<size_t>(selectedMaterialSlot - 1);
                if (slotIndex >= obj.additionalMaterialPaths.size()) {
                    ImGui::TextDisabled("Selected material slot is no longer available.");
                } else {
                    std::string& slotPath = obj.additionalMaterialPaths[slotIndex];
                    if (slotPath.empty()) {
                        ImGui::TextDisabled("Assign a material asset to this slot in Renderer to edit it here.");
                    } else {
                        if (!slotMaterialInspectorValid) {
                            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                               "Failed to read material file for this slot.");
                        } else {
                            const bool slotMaterialChanged = renderMaterialEditorBody(
                                "SlotMaterial",
                                slotInspectedMaterial,
                                slotInspectedAlbedo,
                                slotInspectedOverlay,
                                slotInspectedNormal,
                                slotInspectedMetallicRoughness,
                                slotInspectedMetallicMap,
                                slotInspectedRoughnessMap,
                                slotInspectedOpacityMap,
                                slotInspectedEmissive,
                                slotInspectedOcclusion,
                                slotInspectedHeight,
                                slotInspectedUseOverlay,
                                slotInspectedShaderPack,
                                slotInspectedVertShader,
                                slotInspectedFragShader,
                                nullptr,
                                objectMaterialPreviewScale,
                                1003
                            );

                            // Reload / Save moved to the Material header's right-click menu, so
                            // slot edits save themselves like the primary material's do -
                            // otherwise nothing would ever write them.
                            static std::string pendingSlotMaterialSavePath;
                            if (slotMaterialChanged) {
                                slotMaterialInspectorValid = true;
                                propagateMaterialAsset(slotPath, slotMaterialFileData());
                                pendingSlotMaterialSavePath = slotPath;
                            }
                            if (pendingSlotMaterialSavePath == slotPath && !ImGui::IsAnyItemActive()) {
                                pendingSlotMaterialSavePath.clear();
                                saveSlotMaterial(slotPath);
                            }
                        }
                    }
                }
            }
        }
    }

    auto forEachSecondarySelected = [&](auto&& fn) {
        for (SceneObject* selectedObj : selectedObjects) {
            if (!selectedObj || selectedObj->id == obj.id) continue;
            fn(*selectedObj);
        }
    };

    auto applyInspectorFrameDeltaTo = [&](SceneObject& target) {
        if (inspectorPrimaryBefore == nullptr) {
            return;
        }

        const SceneObject& before = *inspectorPrimaryBefore;

        auto animationClipSlotsEqual = [](const std::vector<AnimationClipSlot>& a,
                                          const std::vector<AnimationClipSlot>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].name != b[i].name || a[i].assetPath != b[i].assetPath) return false;
            }
            return true;
        };
        auto animationKeyframesEqual = [](const std::vector<AnimationKeyframe>& a,
                                          const std::vector<AnimationKeyframe>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].time != b[i].time ||
                    a[i].position != b[i].position ||
                    a[i].rotation != b[i].rotation ||
                    a[i].scale != b[i].scale ||
                    a[i].interpolation != b[i].interpolation ||
                    a[i].curveMode != b[i].curveMode ||
                    a[i].bezierIn != b[i].bezierIn ||
                    a[i].bezierOut != b[i].bezierOut) {
                    return false;
                }
            }
            return true;
        };
        auto animationEventsEqual = [](const std::vector<AnimationEvent>& a,
                                       const std::vector<AnimationEvent>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].time != b[i].time ||
                    a[i].eventId != b[i].eventId ||
                    a[i].payload != b[i].payload) {
                    return false;
                }
            }
            return true;
        };
        auto animationPropertyKeyframesEqual = [](const std::vector<AnimationPropertyKeyframe>& a,
                                                 const std::vector<AnimationPropertyKeyframe>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].time != b[i].time ||
                    a[i].value != b[i].value ||
                    a[i].interpolation != b[i].interpolation ||
                    a[i].curveMode != b[i].curveMode ||
                    a[i].bezierIn != b[i].bezierIn ||
                    a[i].bezierOut != b[i].bezierOut) {
                    return false;
                }
            }
            return true;
        };
        auto animationTracksEqual = [&](const std::vector<AnimationPropertyTrack>& a,
                                        const std::vector<AnimationPropertyTrack>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].enabled != b[i].enabled ||
                    a[i].path != b[i].path ||
                    a[i].label != b[i].label ||
                    a[i].defaultValue != b[i].defaultValue ||
                    !animationPropertyKeyframesEqual(a[i].keyframes, b[i].keyframes)) {
                    return false;
                }
            }
            return true;
        };

        #define APPLY_CHANGED_FIELD(FIELD) \
            do { if (before.FIELD != obj.FIELD) target.FIELD = obj.FIELD; } while (false)
        #define APPLY_CHANGED_COMPONENT_FIELD(COMPONENT, FIELD) \
            do { if (before.COMPONENT.FIELD != obj.COMPONENT.FIELD) target.COMPONENT.FIELD = obj.COMPONENT.FIELD; } while (false)
        #define APPLY_CHANGED_MATERIAL_FIELD(FIELD) \
            do { if (before.material.FIELD != obj.material.FIELD) target.material.FIELD = obj.material.FIELD; } while (false)
        #define APPLY_CHANGED_UI_FIELD(FIELD) \
            do { if (before.ui.FIELD != obj.ui.FIELD) target.ui.FIELD = obj.ui.FIELD; } while (false)

        APPLY_CHANGED_FIELD(name);
        APPLY_CHANGED_FIELD(enabled);
        APPLY_CHANGED_FIELD(IsInvariable);
        APPLY_CHANGED_FIELD(layer);
        APPLY_CHANGED_FIELD(tag);
        APPLY_CHANGED_FIELD(editorIconTint);
        APPLY_CHANGED_FIELD(editorIconPath);
        APPLY_CHANGED_FIELD(editorIconShowInViewport);
        if (!target.IsInvariable) {
            APPLY_CHANGED_FIELD(position);
            APPLY_CHANGED_FIELD(rotation);
            APPLY_CHANGED_FIELD(scale);
            APPLY_CHANGED_FIELD(localPosition);
            APPLY_CHANGED_FIELD(localRotation);
            APPLY_CHANGED_FIELD(localScale);
            APPLY_CHANGED_FIELD(localInitialized);
        }
        APPLY_CHANGED_FIELD(parentId);
        APPLY_CHANGED_FIELD(childIds);
        APPLY_CHANGED_FIELD(isExpanded);

        if (sharedUIObject) {
            APPLY_CHANGED_FIELD(hasUI);
            APPLY_CHANGED_UI_FIELD(type);
            APPLY_CHANGED_UI_FIELD(anchor);
            if (!target.IsInvariable) {
                APPLY_CHANGED_UI_FIELD(position);
                APPLY_CHANGED_UI_FIELD(size);
                APPLY_CHANGED_UI_FIELD(rotation);
            }
            APPLY_CHANGED_UI_FIELD(maskChildren);
            APPLY_CHANGED_UI_FIELD(clipChildren);
            APPLY_CHANGED_UI_FIELD(fitWidth);
            APPLY_CHANGED_UI_FIELD(fitHeight);
            APPLY_CHANGED_UI_FIELD(responsive);
            APPLY_CHANGED_UI_FIELD(checked);
            APPLY_CHANGED_UI_FIELD(multiline);
            APPLY_CHANGED_UI_FIELD(password);
            APPLY_CHANGED_UI_FIELD(indeterminate);
            APPLY_CHANGED_UI_FIELD(vertical);
            APPLY_CHANGED_UI_FIELD(virtualize);
            APPLY_CHANGED_UI_FIELD(sortable);
            APPLY_CHANGED_UI_FIELD(open);
            APPLY_CHANGED_UI_FIELD(dimBackground);
            APPLY_CHANGED_UI_FIELD(gridColumns);
            APPLY_CHANGED_UI_FIELD(selectedIndex);
            APPLY_CHANGED_UI_FIELD(linkedObjectId);
            APPLY_CHANGED_UI_FIELD(flexibleSize);
            APPLY_CHANGED_UI_FIELD(aspectRatio);
            APPLY_CHANGED_UI_FIELD(rounding);
            APPLY_CHANGED_UI_FIELD(borderWidth);
            APPLY_CHANGED_UI_FIELD(rangeUpper);
            APPLY_CHANGED_UI_FIELD(rowHeight);
            APPLY_CHANGED_UI_FIELD(numericStep);
            APPLY_CHANGED_UI_FIELD(spacing);
            APPLY_CHANGED_UI_FIELD(minimumSize);
            APPLY_CHANGED_UI_FIELD(preferredSize);
            APPLY_CHANGED_UI_FIELD(maximumSize);
            APPLY_CHANGED_UI_FIELD(anchorMin);
            APPLY_CHANGED_UI_FIELD(anchorMax);
            APPLY_CHANGED_UI_FIELD(pivot);
            APPLY_CHANGED_UI_FIELD(scrollOffset);
            APPLY_CHANGED_UI_FIELD(contentSize);
            APPLY_CHANGED_UI_FIELD(padding);
            APPLY_CHANGED_UI_FIELD(margin);
            APPLY_CHANGED_UI_FIELD(numericValue);
            APPLY_CHANGED_UI_FIELD(inputText);
            APPLY_CHANGED_UI_FIELD(placeholder);
            APPLY_CHANGED_UI_FIELD(tooltip);
            APPLY_CHANGED_UI_FIELD(items);
            APPLY_CHANGED_UI_FIELD(itemParents);
            APPLY_CHANGED_UI_FIELD(expandedItems);
            APPLY_CHANGED_UI_FIELD(layout);
            APPLY_CHANGED_UI_FIELD(sliderValue);
            APPLY_CHANGED_UI_FIELD(sliderMin);
            APPLY_CHANGED_UI_FIELD(sliderMax);
            APPLY_CHANGED_UI_FIELD(label);
            APPLY_CHANGED_UI_FIELD(buttonPressed);
            APPLY_CHANGED_UI_FIELD(color);
            APPLY_CHANGED_UI_FIELD(interactable);
            APPLY_CHANGED_UI_FIELD(sliderStyle);
            APPLY_CHANGED_UI_FIELD(buttonStyle);
            APPLY_CHANGED_UI_FIELD(stylePreset);
            APPLY_CHANGED_UI_FIELD(textScale);
            APPLY_CHANGED_UI_FIELD(textFont);
            APPLY_CHANGED_UI_FIELD(textAutoWrap);
            APPLY_CHANGED_UI_FIELD(textHAlign);
            APPLY_CHANGED_UI_FIELD(textVAlign);
            APPLY_CHANGED_UI_FIELD(textEffectFlags);
            APPLY_CHANGED_UI_FIELD(textEffectSpeed);
            APPLY_CHANGED_UI_FIELD(textEffectIntensity);
            APPLY_CHANGED_UI_FIELD(renderIn3D);
            APPLY_CHANGED_UI_FIELD(renderTargetSize);
            APPLY_CHANGED_UI_FIELD(renderTargetFilter);
            APPLY_CHANGED_UI_FIELD(pseudo3DEnabled);
            APPLY_CHANGED_UI_FIELD(pseudo3DUseOffscreenSurface);
            APPLY_CHANGED_UI_FIELD(pseudo3DPanelSize);
            APPLY_CHANGED_UI_FIELD(pseudo3DTopLeftOffset);
            APPLY_CHANGED_UI_FIELD(pseudo3DTopRightOffset);
            APPLY_CHANGED_UI_FIELD(pseudo3DBottomRightOffset);
            APPLY_CHANGED_UI_FIELD(pseudo3DBottomLeftOffset);
            APPLY_CHANGED_UI_FIELD(pseudo3DPivot);
            APPLY_CHANGED_UI_FIELD(pseudo3DPerspectiveIntensity);
            APPLY_CHANGED_UI_FIELD(pseudo3DSkewAmount);
            APPLY_CHANGED_UI_FIELD(pseudo3DCurvatureAmount);
            APPLY_CHANGED_UI_FIELD(pseudo3DAnchorTargetId);
            APPLY_CHANGED_UI_FIELD(pseudo3DDistanceScalingEnabled);
            APPLY_CHANGED_UI_FIELD(pseudo3DAdjustPerspectiveWithDistance);
            APPLY_CHANGED_UI_FIELD(pseudo3DMinDistance);
            APPLY_CHANGED_UI_FIELD(pseudo3DMaxDistance);
            APPLY_CHANGED_UI_FIELD(pseudo3DInteractionDistance);
            APPLY_CHANGED_UI_FIELD(pseudo3DDepthSort);
            APPLY_CHANGED_UI_FIELD(pseudo3DAllowInteraction);
            APPLY_CHANGED_UI_FIELD(spriteSheetEnabled);
            APPLY_CHANGED_UI_FIELD(spriteSheetColumns);
            APPLY_CHANGED_UI_FIELD(spriteSheetRows);
            APPLY_CHANGED_UI_FIELD(spriteSheetFrame);
            APPLY_CHANGED_UI_FIELD(spriteSheetFps);
            APPLY_CHANGED_UI_FIELD(spriteSheetLoop);
            APPLY_CHANGED_UI_FIELD(spriteCustomFramesEnabled);
            APPLY_CHANGED_UI_FIELD(spriteSourceWidth);
            APPLY_CHANGED_UI_FIELD(spriteSourceHeight);
            APPLY_CHANGED_UI_FIELD(spriteCustomFrames);
            APPLY_CHANGED_UI_FIELD(spriteCustomFrameNames);
            APPLY_CHANGED_UI_FIELD(spriteCustomFrameScales);
            APPLY_CHANGED_UI_FIELD(nineSliceEnabled);
            APPLY_CHANGED_UI_FIELD(nineSliceBorder);
            APPLY_CHANGED_UI_FIELD(nineSliceTileEdges);
            APPLY_CHANGED_UI_FIELD(nineSliceTileCenter);
            APPLY_CHANGED_UI_FIELD(receiveLighting2D);
            APPLY_CHANGED_UI_FIELD(unlitLighting2D);
            APPLY_CHANGED_UI_FIELD(emissiveLighting2D);
            APPLY_CHANGED_FIELD(albedoTexturePath);
            APPLY_CHANGED_FIELD(materialPath);
            APPLY_CHANGED_FIELD(overlayTexturePath);
            APPLY_CHANGED_FIELD(normalMapPath);
            APPLY_CHANGED_FIELD(metallicRoughnessMapPath);
            APPLY_CHANGED_FIELD(metallicMapPath);
            APPLY_CHANGED_FIELD(roughnessMapPath);
            APPLY_CHANGED_FIELD(opacityMapPath);
            APPLY_CHANGED_FIELD(emissiveMapPath);
            APPLY_CHANGED_FIELD(occlusionMapPath);
            APPLY_CHANGED_FIELD(heightMapPath);
            APPLY_CHANGED_FIELD(shaderPackPath);
            APPLY_CHANGED_FIELD(vertexShaderPath);
            APPLY_CHANGED_FIELD(fragmentShaderPath);
            APPLY_CHANGED_FIELD(useOverlay);
            APPLY_CHANGED_MATERIAL_FIELD(color);
            APPLY_CHANGED_MATERIAL_FIELD(alpha);
            APPLY_CHANGED_MATERIAL_FIELD(ambientStrength);
            APPLY_CHANGED_MATERIAL_FIELD(specularStrength);
            APPLY_CHANGED_MATERIAL_FIELD(shininess);
            APPLY_CHANGED_MATERIAL_FIELD(normalMapIntensity);
            APPLY_CHANGED_MATERIAL_FIELD(textureMix);
            APPLY_CHANGED_MATERIAL_FIELD(uvTiling);
            APPLY_CHANGED_MATERIAL_FIELD(uvOffset);
            APPLY_CHANGED_MATERIAL_FIELD(scrollSpeed);
            APPLY_CHANGED_MATERIAL_FIELD(scrollDirection);
            APPLY_CHANGED_MATERIAL_FIELD(uvScrollEnabled);
            APPLY_CHANGED_MATERIAL_FIELD(cloudColor);
            APPLY_CHANGED_MATERIAL_FIELD(cloudSkyColor);
            APPLY_CHANGED_MATERIAL_FIELD(cloudScale);
            APPLY_CHANGED_MATERIAL_FIELD(cloudCoverage);
            APPLY_CHANGED_MATERIAL_FIELD(cloudSoftness);
            APPLY_CHANGED_MATERIAL_FIELD(cloudDetail);
            APPLY_CHANGED_MATERIAL_FIELD(cloudSpeed);
            APPLY_CHANGED_MATERIAL_FIELD(cloudWarp);
            APPLY_CHANGED_MATERIAL_FIELD(cloudHighlight);
            APPLY_CHANGED_MATERIAL_FIELD(cloudStars);
            APPLY_CHANGED_MATERIAL_FIELD(cloudHorizon);
            APPLY_CHANGED_MATERIAL_FIELD(textureFilter);
        }

        if (sharedCollider) {
            APPLY_CHANGED_FIELD(hasCollider);
            APPLY_CHANGED_COMPONENT_FIELD(collider, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(collider, type);
            APPLY_CHANGED_COMPONENT_FIELD(collider, boxSize);
            APPLY_CHANGED_COMPONENT_FIELD(collider, offset);
            APPLY_CHANGED_COMPONENT_FIELD(collider, convex);
            APPLY_CHANGED_COMPONENT_FIELD(collider, isTrigger);
            APPLY_CHANGED_COMPONENT_FIELD(collider, staticFriction);
            APPLY_CHANGED_COMPONENT_FIELD(collider, dynamicFriction);
            APPLY_CHANGED_COMPONENT_FIELD(collider, restitution);
        }

        if (sharedPlayerController) {
            APPLY_CHANGED_FIELD(hasPlayerController);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, moveSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, runSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, lookSensitivity);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, groundAcceleration);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, airAcceleration);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, braking);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, minSurfaceControl);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, slideGravity);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, platformCarry);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, height);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, radius);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, jumpStrength);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, verticalVelocity);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, pitch);
            APPLY_CHANGED_COMPONENT_FIELD(playerController, yaw);
        }

        if (sharedRigidbody) {
            APPLY_CHANGED_FIELD(hasRigidbody);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, mass);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, useCustomCenterOfMass);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, centerOfMass);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, useGravity);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, isKinematic);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, linearDamping);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, angularDamping);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, interpolationMode);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, maxExtrapolationTime);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, lockRotationX);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, lockRotationY);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody, lockRotationZ);
        }

        if (sharedRigidbody2D) {
            APPLY_CHANGED_FIELD(hasRigidbody2D);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody2D, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody2D, useGravity);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody2D, lockRotation);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody2D, gravityScale);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody2D, linearDamping);
            APPLY_CHANGED_COMPONENT_FIELD(rigidbody2D, velocity);
        }

        if (sharedCollider2D) {
            APPLY_CHANGED_FIELD(hasCollider2D);
            APPLY_CHANGED_COMPONENT_FIELD(collider2D, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(collider2D, isTrigger);
            APPLY_CHANGED_COMPONENT_FIELD(collider2D, type);
            APPLY_CHANGED_COMPONENT_FIELD(collider2D, boxSize);
            APPLY_CHANGED_COMPONENT_FIELD(collider2D, offset);
            APPLY_CHANGED_COMPONENT_FIELD(collider2D, points);
            APPLY_CHANGED_COMPONENT_FIELD(collider2D, closed);
            APPLY_CHANGED_COMPONENT_FIELD(collider2D, edgeThickness);
        }

        if (sharedParallax2D) {
            APPLY_CHANGED_FIELD(hasParallaxLayer2D);
            APPLY_CHANGED_COMPONENT_FIELD(parallaxLayer2D, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(parallaxLayer2D, order);
            APPLY_CHANGED_COMPONENT_FIELD(parallaxLayer2D, factor);
            APPLY_CHANGED_COMPONENT_FIELD(parallaxLayer2D, repeatX);
            APPLY_CHANGED_COMPONENT_FIELD(parallaxLayer2D, repeatY);
            APPLY_CHANGED_COMPONENT_FIELD(parallaxLayer2D, disableCulling);
            APPLY_CHANGED_COMPONENT_FIELD(parallaxLayer2D, repeatSpacing);
        }

        if (sharedAudioSource) {
            APPLY_CHANGED_FIELD(hasAudioSource);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, clipPath);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, volume);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, loop);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, playOnStart);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, spatial);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, spatialBlend);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, minDistance);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, maxDistance);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, rolloffMode);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, rolloff);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, customMidDistance);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, customMidGain);
            APPLY_CHANGED_COMPONENT_FIELD(audioSource, customEndGain);
        }

        if (sharedVideoPlayer) {
            APPLY_CHANGED_FIELD(hasVideoPlayer);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, videoPath);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, playOnAwake);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, loop);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, flipX);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, flipY);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, playbackSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, playAudioFromVideo);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, routeAudioToSource);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, outputAudioSourceObjectId);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, videoAudioVolume);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, videoAudioMuted);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, syncAudioToVideo);
            APPLY_CHANGED_COMPONENT_FIELD(videoPlayer, audioSyncTolerance);
        }

        if (sharedParticleSystem2D) {
            APPLY_CHANGED_FIELD(hasParticleSystem2D);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, looping);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, prewarm);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, playOnAwake);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, autoRandomSeed);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, randomSeed);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, startDelay);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, startLifetime);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, startSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, startSize);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, startRotation);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, startColor);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, gravityModifier);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, simulationSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, maxParticles);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, emissionRate);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, burstCount);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, burstTime);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, burstLoop);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, shape);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, shapeRadius);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, shapeBox);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, velocityOverLifetimeEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, velocityOverLifetime);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, colorOverLifetimeEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, colorOverLifetime);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, sizeOverLifetimeEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, sizeOverLifetime);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, rotationOverLifetimeEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, rotationOverLifetime);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, noiseEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, noiseStrength);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, noiseFrequency);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, texturePath);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, materialPath);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, receiveLighting2D);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, unlitLighting2D);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, emissiveLighting2D);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, emitIn3D);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, simulationSpace);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, shapeAngle);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, renderAlignment);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, stretchLengthScale);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, stretchSpeedScale);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, blendMode);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetMode);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetTilesX);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetTilesY);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetTiming);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetCycles);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetFps);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetLoop);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetStartFrame);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetFrameCount);
            APPLY_CHANGED_COMPONENT_FIELD(particleSystem2D, textureSheetRandomStart);
        }

        if (sharedGroundBaked) {
            APPLY_CHANGED_FIELD(hasGroundBakedType);
            APPLY_CHANGED_COMPONENT_FIELD(groundBakedType, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(groundBakedType, includeInBake);
            APPLY_CHANGED_COMPONENT_FIELD(groundBakedType, areaCost);
        }

        if (sharedObstacle) {
            APPLY_CHANGED_FIELD(hasObsticleObject);
            APPLY_CHANGED_COMPONENT_FIELD(obsticleObject, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(obsticleObject, carve);
            APPLY_CHANGED_COMPONENT_FIELD(obsticleObject, padding);
        }

        if (sharedAgent) {
            APPLY_CHANGED_FIELD(hasAIAgent);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, useTargetObject);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, targetId);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, destination);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, speed);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, stoppingDistance);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, repathInterval);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, autoRepath);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, alignToPath);
            APPLY_CHANGED_COMPONENT_FIELD(aiAgent, debugDrawPath);
        }

        if (sharedAnimation) {
            APPLY_CHANGED_FIELD(hasAnimation);
            APPLY_CHANGED_COMPONENT_FIELD(animation, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(animation, clipAssetPath);
            if (!animationClipSlotsEqual(before.animation.clips, obj.animation.clips)) {
                target.animation.clips = obj.animation.clips;
            }
            APPLY_CHANGED_COMPONENT_FIELD(animation, activeClipIndex);
            APPLY_CHANGED_COMPONENT_FIELD(animation, clipLength);
            APPLY_CHANGED_COMPONENT_FIELD(animation, playSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(animation, loop);
            APPLY_CHANGED_COMPONENT_FIELD(animation, playOnAwake);
            APPLY_CHANGED_COMPONENT_FIELD(animation, applyOnScrub);
            if (!animationKeyframesEqual(before.animation.keyframes, obj.animation.keyframes)) {
                target.animation.keyframes = obj.animation.keyframes;
            }
            if (!animationEventsEqual(before.animation.events, obj.animation.events)) {
                target.animation.events = obj.animation.events;
            }
            if (!animationTracksEqual(before.animation.tracks, obj.animation.tracks)) {
                target.animation.tracks = obj.animation.tracks;
            }
        }

        if (sharedSkeletal) {
            APPLY_CHANGED_FIELD(hasSkeletalAnimation);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, useGpuSkinning);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, allowCpuFallback);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, useAnimation);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, clipIndex);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, time);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, playSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, loop);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, skeletonRootId);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, maxBones);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, boneNames);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, boneNodeIds);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, armatureNodeIds);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, inverseBindMatrices);
            APPLY_CHANGED_COMPONENT_FIELD(skeletal, finalMatrices);
        }

        if (sharedReverb) {
            APPLY_CHANGED_FIELD(hasReverbZone);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, preset);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, shape);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, boxSize);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, radius);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, blendDistance);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, minDistance);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, maxDistance);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, room);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, roomHF);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, roomLF);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, decayTime);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, decayHFRatio);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, reflections);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, reflectionsDelay);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, reverb);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, reverbDelay);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, hfReference);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, lfReference);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, roomRolloffFactor);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, diffusion);
            APPLY_CHANGED_COMPONENT_FIELD(reverbZone, density);
        }

        if (sharedCamera) {
            APPLY_CHANGED_FIELD(hasCamera);
            APPLY_CHANGED_COMPONENT_FIELD(camera, type);
            APPLY_CHANGED_COMPONENT_FIELD(camera, fov);
            APPLY_CHANGED_COMPONENT_FIELD(camera, nearClip);
            APPLY_CHANGED_COMPONENT_FIELD(camera, farClip);
            APPLY_CHANGED_COMPONENT_FIELD(camera, applyPostFX);
            APPLY_CHANGED_COMPONENT_FIELD(camera, use2D);
            APPLY_CHANGED_COMPONENT_FIELD(camera, pixelsPerUnit);
        }

        if (sharedCameraFollow2D) {
            APPLY_CHANGED_FIELD(hasCameraFollow2D);
            APPLY_CHANGED_COMPONENT_FIELD(cameraFollow2D, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(cameraFollow2D, targetId);
            APPLY_CHANGED_COMPONENT_FIELD(cameraFollow2D, offset);
            APPLY_CHANGED_COMPONENT_FIELD(cameraFollow2D, smoothTime);
        }

        if (sharedPostFX) {
            APPLY_CHANGED_FIELD(hasPostFX);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, isGlobal);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, priority);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, blendWeight);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, distortionPolarization);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, blendRadius);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, hdrEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, toneMapper);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, whitePoint);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, gamma);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, bloomEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, bloomThreshold);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, bloomSoftKnee);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, bloomIntensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, bloomRadius);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, colorAdjustEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, exposure);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, contrast);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, saturation);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, colorFilter);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, eyeAdaptationEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, eyeAdaptationMinEv);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, eyeAdaptationMaxEv);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, eyeAdaptationSpeedUp);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, eyeAdaptationSpeedDown);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, eyeAdaptationKeyValue);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, purkinjeEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, purkinjeStrength);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, purkinjeThreshold);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, purkinjeRange);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, purkinjeTint);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, motionBlurEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, motionBlurStrength);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, motionBlurThreshold);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, motionBlurClamp);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vignetteEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareIntensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareThreshold);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareGhosts);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareGhostSpacing);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareHaloRadius);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareStreak);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareTint);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareGhostsEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareHaloEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareStreakEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareGlowEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareGlowSize);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensFlareGlowIntensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vignetteIntensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vignetteSmoothness);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, chromaticAberrationEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, chromaticAmount);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, sharpenEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, sharpenStrength);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ambientOcclusionEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, aoRadius);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, aoStrength);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherIntensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherColorBits);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherDarkAdjustment);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherPixelation);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherSize);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherContrast);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherOffset);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherPalette);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, ditherPattern);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticIntensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticGrainScale);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticDarkAreaInfluence);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticMonochrome);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticSparkle);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticDistortionEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticDistortionHorizontalJitterAmount);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticDistortionLineDensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticDistortionGlitchFrequency);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, staticDistortionStrength);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensDistortionEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensDistortionAmount);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensDistortionEdgeFalloff);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensDistortionCenterOffset);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensDistortionEdgeVignetteEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensDistortionEdgeVignetteIntensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensDistortionEdgeVignetteRadius);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensDistortionEdgeVignetteSoftness);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, lensDistortionEdgeVignetteColor);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, pixelationEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, pixelationSize);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, posterizeEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, posterizeLevels);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, scanlinesEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, scanlinesIntensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, scanlinesDensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, scanlinesSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayOpacity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayScanlineStrength);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayTapeNoise);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayChromaBleed);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayBottomNoiseBandHeight);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayBottomNoiseBandIntensity);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayDistortionStrength);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayAnimationSpeed);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayColorBleed);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayBanding);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlaySignalMode);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, vhsOverlayDropouts);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, wavyEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, wavyAmplitude);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, wavyFrequency);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, wavySpeed);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, wavyVertical);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, scope2DEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, scope2DMode);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, scope2DMinOrder);
            APPLY_CHANGED_COMPONENT_FIELD(postFx, scope2DMaxOrder);
        }

        if (sharedRenderer) {
            APPLY_CHANGED_FIELD(hasRenderer);
            APPLY_CHANGED_FIELD(renderType);
            APPLY_CHANGED_FIELD(faceCamera);
            APPLY_CHANGED_FIELD(meshPath);
            APPLY_CHANGED_FIELD(meshId);
            APPLY_CHANGED_FIELD(meshSourceIndex);
            APPLY_CHANGED_MATERIAL_FIELD(color);
            APPLY_CHANGED_MATERIAL_FIELD(alpha);
            APPLY_CHANGED_MATERIAL_FIELD(ambientStrength);
            APPLY_CHANGED_MATERIAL_FIELD(specularStrength);
            APPLY_CHANGED_MATERIAL_FIELD(shininess);
            APPLY_CHANGED_MATERIAL_FIELD(normalMapIntensity);
            APPLY_CHANGED_MATERIAL_FIELD(textureMix);
            APPLY_CHANGED_MATERIAL_FIELD(uvTiling);
            APPLY_CHANGED_MATERIAL_FIELD(uvOffset);
            APPLY_CHANGED_MATERIAL_FIELD(scrollSpeed);
            APPLY_CHANGED_MATERIAL_FIELD(scrollDirection);
            APPLY_CHANGED_MATERIAL_FIELD(uvScrollEnabled);
            APPLY_CHANGED_MATERIAL_FIELD(cloudColor);
            APPLY_CHANGED_MATERIAL_FIELD(cloudSkyColor);
            APPLY_CHANGED_MATERIAL_FIELD(cloudScale);
            APPLY_CHANGED_MATERIAL_FIELD(cloudCoverage);
            APPLY_CHANGED_MATERIAL_FIELD(cloudSoftness);
            APPLY_CHANGED_MATERIAL_FIELD(cloudDetail);
            APPLY_CHANGED_MATERIAL_FIELD(cloudSpeed);
            APPLY_CHANGED_MATERIAL_FIELD(cloudWarp);
            APPLY_CHANGED_MATERIAL_FIELD(cloudHighlight);
            APPLY_CHANGED_MATERIAL_FIELD(cloudStars);
            APPLY_CHANGED_MATERIAL_FIELD(cloudHorizon);
            APPLY_CHANGED_MATERIAL_FIELD(textureFilter);
            APPLY_CHANGED_FIELD(materialPath);
            APPLY_CHANGED_FIELD(albedoTexturePath);
            APPLY_CHANGED_FIELD(overlayTexturePath);
            APPLY_CHANGED_FIELD(normalMapPath);
            APPLY_CHANGED_FIELD(shaderPackPath);
            APPLY_CHANGED_FIELD(vertexShaderPath);
            APPLY_CHANGED_FIELD(fragmentShaderPath);
            APPLY_CHANGED_FIELD(useOverlay);
            APPLY_CHANGED_FIELD(additionalMaterialPaths);
        }

        if (sharedLight) {
            APPLY_CHANGED_FIELD(hasLight);
            APPLY_CHANGED_COMPONENT_FIELD(light, type);
            APPLY_CHANGED_COMPONENT_FIELD(light, color);
            APPLY_CHANGED_COMPONENT_FIELD(light, intensity);
            APPLY_CHANGED_COMPONENT_FIELD(light, range);
            APPLY_CHANGED_COMPONENT_FIELD(light, edgeFade);
            APPLY_CHANGED_COMPONENT_FIELD(light, innerAngle);
            APPLY_CHANGED_COMPONENT_FIELD(light, outerAngle);
            APPLY_CHANGED_COMPONENT_FIELD(light, size);
            APPLY_CHANGED_COMPONENT_FIELD(light, castShadows);
            APPLY_CHANGED_COMPONENT_FIELD(light, softShadows);
            APPLY_CHANGED_COMPONENT_FIELD(light, shadowBias);
            APPLY_CHANGED_COMPONENT_FIELD(light, shadowSoftness);
            APPLY_CHANGED_COMPONENT_FIELD(light, shadowResolution);
            APPLY_CHANGED_COMPONENT_FIELD(light, enabled);
        }

        if (sharedLight2D) {
            APPLY_CHANGED_FIELD(hasLight2D);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, type);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, color);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, intensity);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, radius);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, innerRadius);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, outerRadius);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, falloffStrength);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, innerSpotAngle);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, outerSpotAngle);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, blendStyle);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, lightOrder);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, overlapOperation);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, shadowStrength);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, volumetricEnabled);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, castsShadows);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, targetAllLayers);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, targetLayerMask);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, normalMapQuality);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, normalMapDistance);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, useDistanceExponent);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, distanceExponent);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, cookieTexturePath);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, cookieScale);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, cookieRotation);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, freeformFeather);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, freeformEdgeFalloff);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, shapePoints);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, flicker.enabled);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, flicker.speed);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, flicker.amount);
            APPLY_CHANGED_COMPONENT_FIELD(light2D, flicker.seed);
        }

        if (sharedShadowCaster2D) {
            APPLY_CHANGED_FIELD(hasShadowCaster2D);
            APPLY_CHANGED_COMPONENT_FIELD(shadowCaster2D, enabled);
            APPLY_CHANGED_COMPONENT_FIELD(shadowCaster2D, castsSelfShadow);
            APPLY_CHANGED_COMPONENT_FIELD(shadowCaster2D, targetAllLayers);
            APPLY_CHANGED_COMPONENT_FIELD(shadowCaster2D, targetLayerMask);
            APPLY_CHANGED_COMPONENT_FIELD(shadowCaster2D, shadowStrength);
            APPLY_CHANGED_COMPONENT_FIELD(shadowCaster2D, points);
        }

        #undef APPLY_CHANGED_UI_FIELD
        #undef APPLY_CHANGED_MATERIAL_FIELD
        #undef APPLY_CHANGED_COMPONENT_FIELD
        #undef APPLY_CHANGED_FIELD
    };

    if (multiSelection) {
        bool propagated = false;

        if (objectNameChanged || objectEnabledChanged || objectInvariableChanged ||
            objectIconChanged ||
            objectLayerChanged || objectTagChanged || objectTransformChanged) {
            forEachSecondarySelected([&](SceneObject& target) {
                const std::string oldTargetName = target.name;
                applyInspectorFrameDeltaTo(target);
                if (objectNameChanged && oldTargetName != target.name) {
                    propagateObjectRenameReferences(oldTargetName, target.name, target.id);
                }
                if (objectTransformChanged && !(target.parentId != -1 && target.localInitialized)) {
                    syncLocalTransform(target);
                }
            });
            if (objectTransformChanged) {
                updateHierarchyWorldTransforms();
            }
            propagated = true;
        }

        if (uiSectionChanged && sharedUIObject) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (colliderSectionChanged && sharedCollider) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (playerControllerSectionChanged && sharedPlayerController) {
            forEachSecondarySelected([&](SceneObject& target) {
                applyInspectorFrameDeltaTo(target);
                if (target.hasPlayerController) {
                    target.hasCollider = true;
                    target.collider.type = ColliderType::Capsule;
                    target.collider.convex = true;
                    target.hasRigidbody = true;
                    target.rigidbody.enabled = true;
                    target.rigidbody.useGravity = true;
                    target.rigidbody.lockRotationX = true;
                    target.rigidbody.lockRotationY = false;
                    target.rigidbody.lockRotationZ = true;
                }
            });
            propagated = true;
        }
        if (rigidbodySectionChanged && sharedRigidbody) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (rigidbody2DSectionChanged && sharedRigidbody2D) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (collider2DSectionChanged && sharedCollider2D) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (parallax2DSectionChanged && sharedParallax2D) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (audioSourceSectionChanged && sharedAudioSource) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (audioFXSectionChanged && sharedAudioFX) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (videoPlayerSectionChanged && sharedVideoPlayer) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (particleSystem2DSectionChanged && sharedParticleSystem2D) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (groundBakedSectionChanged && sharedGroundBaked) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (obstacleSectionChanged && sharedObstacle) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (agentSectionChanged && sharedAgent) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (animationSectionChanged && sharedAnimation) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (skeletalSectionChanged && sharedSkeletal) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (reverbSectionChanged && sharedReverb) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (cameraSectionChanged && sharedCamera) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (cameraFollowSectionChanged && sharedCameraFollow2D) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (postFxSectionChanged && sharedPostFX) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (rendererSectionChanged && sharedRenderer) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (lightSectionChanged && sharedLight) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (light2DSectionChanged && sharedLight2D) {
            forEachSecondarySelected([&](SceneObject& target) {
                applyInspectorFrameDeltaTo(target);
                lighting2DRenderer.clearPolygonCache(target.id);
            });
            propagated = true;
        }
        if (shadowCaster2DSectionChanged && sharedShadowCaster2D) {
            forEachSecondarySelected(applyInspectorFrameDeltaTo);
            propagated = true;
        }
        if (scriptsChanged) {
            if (sharedScriptsLayout) {
                forEachSecondarySelected([&](SceneObject& target) {
                    target.scripts = obj.scripts;
                });
                propagated = true;
            } else {
                forEachSecondarySelected([&](SceneObject& target) {
                    for (size_t i = 0; i < sharedScriptByIndex.size() && i < obj.scripts.size(); ++i) {
                        if (!sharedScriptByIndex[i]) continue;
                        const std::string& oldSignature = sharedScriptSignatures[i];
                        const std::string newSignature = scriptSignature(obj.scripts[i]);
                        auto targetIt = std::find_if(target.scripts.begin(), target.scripts.end(),
                            [&](const ScriptComponent& candidate) {
                                const std::string candidateSignature = scriptSignature(candidate);
                                return candidateSignature == oldSignature || candidateSignature == newSignature;
                            });
                        if (targetIt != target.scripts.end()) {
                            *targetIt = obj.scripts[i];
                        }
                    }
                });
                propagated = true;
            }
        }

        if (propagated) {
            forEachSecondarySelected([&](SceneObject& target) {
                UpdateLegacyTypeFromComponents(target);
            });
            projectManager.currentProject.hasUnsavedChanges = true;
        }
    }

    if (scriptsChanged) {
        markRuntimeScriptBindingsDirty();
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    if (scriptsChanged || componentChanged || inspectorOrderChanged) {
        EnsureInspectorComponentMetadata(obj);
    }
    if (inspectorOrderChanged) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    if (componentChanged) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }

    // A material, script or audio clip dropped anywhere on the Inspector applies to the objects it
    // shows. This target spans the whole panel and is submitted after every field; ImGui hands a
    // drop to the smallest target under the cursor, so a reference slot still takes a drop that
    // lands on it. Runs before the undo bookkeeping below, which records it like any edit.
    {
        std::string inspectorDragPath;
        const ObjectAssetDropKind inspectorDragKind = activeObjectAssetDragKind(&inspectorDragPath);
        bool anyApplicable = false;
        for (const SceneObject* selectedObj : selectedObjects) {
            anyApplicable = anyApplicable || (selectedObj && canApplyAssetDrop(*selectedObj, inspectorDragKind));
        }
        ImGuiWindow* inspectorWindow = ImGui::GetCurrentWindow();
        if (anyApplicable && inspectorWindow &&
            ImGui::BeginDragDropTargetCustom(inspectorWindow->InnerRect, ImGui::GetID("##InspectorAssetDrop"))) {
            const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                "FILE_PATH", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            if (payload && payload->IsPreview()) {
                // Glow round the panel here, and the object's outline in the Scene view.
                setAssetDropHover(obj.id);
                ImDrawList* glowList = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
                const ImRect& glowRect = inspectorWindow->InnerRect;
                glowList->PushClipRect(inspectorWindow->Rect().Min, inspectorWindow->Rect().Max, true);
                for (int ring = 3; ring >= 1; --ring) {
                    const float inset = 2.0f * static_cast<float>(ring);
                    glowList->AddRect(ImVec2(glowRect.Min.x + inset, glowRect.Min.y + inset),
                                      ImVec2(glowRect.Max.x - inset, glowRect.Max.y - inset),
                                      IM_COL32(122, 110, 255, 40 * (4 - ring)), 6.0f, 0, 2.0f);
                }
                glowList->AddRect(glowRect.Min, glowRect.Max, IM_COL32(122, 110, 255, 255), 6.0f, 0, 2.0f);
                const char* action = inspectorDragKind == ObjectAssetDropKind::Material ? "Drop to assign the material"
                                   : inspectorDragKind == ObjectAssetDropKind::Script   ? "Drop to add the script"
                                                                                        : "Drop to assign the audio clip";
                const ImVec2 textSize = ImGui::CalcTextSize(action);
                const ImVec2 labelMin(glowRect.Min.x + (glowRect.GetWidth() - textSize.x) * 0.5f - 8.0f,
                                      glowRect.Max.y - textSize.y - 18.0f);
                const ImVec2 labelMax(labelMin.x + textSize.x + 16.0f, labelMin.y + textSize.y + 8.0f);
                glowList->AddRectFilled(labelMin, labelMax, IM_COL32(20, 22, 32, 230), 4.0f);
                glowList->AddRect(labelMin, labelMax, IM_COL32(122, 110, 255, 255), 4.0f, 0, 1.5f);
                glowList->AddText(ImVec2(labelMin.x + 8.0f, labelMin.y + 4.0f), IM_COL32(236, 240, 246, 255), action);
                glowList->PopClipRect();
            }
            if (payload && payload->IsDelivery()) {
                for (SceneObject* selectedObj : selectedObjects) {
                    if (selectedObj && applyAssetDropToObject(*selectedObj, fs::path(inspectorDragPath), false)) {
                        componentChanged = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    const bool inspectorChanged =
        objectNameChanged || objectEnabledChanged || objectInvariableChanged || objectIconChanged ||
        objectLayerChanged || objectTagChanged || objectTransformChanged || uiSectionChanged || colliderSectionChanged ||
        playerControllerSectionChanged || rigidbodySectionChanged || rigidbody2DSectionChanged ||
        collider2DSectionChanged || parallax2DSectionChanged || audioSourceSectionChanged || audioFXSectionChanged ||
        videoPlayerSectionChanged || particleSystem2DSectionChanged || groundBakedSectionChanged || obstacleSectionChanged ||
        agentSectionChanged || animationSectionChanged || skeletalSectionChanged ||
        reverbSectionChanged || cameraSectionChanged || cameraFollowSectionChanged ||
        postFxSectionChanged || rendererSectionChanged || lightSectionChanged ||
        light2DSectionChanged || shadowCaster2DSectionChanged || scriptsChanged ||
        inspectorOrderChanged || componentChanged;

    if (inspectorChanged && !inspectorHistoryPending && inspectorFrameBeforeValid) {
        inspectorHistoryBefore = std::move(inspectorFrameBefore);
        inspectorHistoryPending = true;
    }
    if (inspectorHistoryPending && !ImGui::IsAnyItemActive()) {
        pushUndoSnapshot(std::move(inspectorHistoryBefore), "inspectorEdit");
        inspectorHistoryBefore = {};
        inspectorHistoryPending = false;
    }

    if (browserHasAudio) {
        ImGui::Spacing();
        renderAudioAssetPanel("Audio Clip (File Browser)", &obj);
    }
    if (browserHasMaterial) {
        ImGui::Spacing();
        renderMaterialAssetPanel("Material Asset (File Browser)", true);
    }
    if (multiSelection && hasMixedComponents) {
        ImGui::Spacing();
        ImGui::TextDisabled("Note: not all components are shown because one or more selected objects have different components/scripts.");
    }

    ImGui::PopID(); // object scope
    if (inspectorWrapText) ImGui::PopTextWrapPos();
    ImGui::PopStyleVar(3);
    if (ImGui::GetScrollMaxX() > 0.0f) {
        ImGui::SetScrollX(0.0f);
    }
    ImGui::End();
    const double __ipMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - __ipStart).count();
    if (__ipMs > Modularity::HardwareProfile::TimingBudget(20.0)) {
        std::fprintf(stderr, "[ModuTimer] inspectorPanel %.2f ms\n", __ipMs);
    }
}

#pragma endregion
