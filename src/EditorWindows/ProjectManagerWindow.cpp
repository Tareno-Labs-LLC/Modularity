#include "Engine.h"
#include "ModelLoader.h"
#include "../EditorLocalization.h"
#include "../ModuCPPLanguagePack.h"
#include "../XR/XRDiagnostics.h"
#include "../XR/XRFeatures.h"
#include "../XR/XRRuntime.h"
#include "../XR/XRRuntimeSelection.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <functional>
#include <cstdint>
#include <sstream>
#include <source_location>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <future>
#include <chrono>
#include <future>

#ifdef _WIN32
#include <shlobj.h>
#endif

#ifdef __ANDROID__
#include "AndroidRuntime/AndroidRuntime.h"
#endif

namespace Loc = Modularity::Loc;

#pragma region ImGui Helpers
namespace ImGui {

// Animated progress bar that keeps circles moving while work happens in the background.
bool BufferingBar(const char* label, float value, const ImVec2& size_arg, const ImU32& bg_col, const ImU32& fg_col) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = size_arg;
    size.x -= style.FramePadding.x * 2;

    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ItemSize(bb, style.FramePadding.y);
    if (!ItemAdd(bb, id))
        return false;

    const float circleStart = size.x * 0.7f;
    const float circleEnd = size.x;
    const float circleWidth = circleEnd - circleStart;

    window->DrawList->AddRectFilled(bb.Min, ImVec2(pos.x + circleStart, bb.Max.y), bg_col);
    window->DrawList->AddRectFilled(bb.Min, ImVec2(pos.x + circleStart * value, bb.Max.y), fg_col);

    const float t = g.Time;
    const float r = size.y / 2;
    const float speed = 1.5f;

    const float a = speed * 0;
    const float b = speed * 0.333f;
    const float c = speed * 0.666f;

    const float o1 = (circleWidth + r) * (t + a - speed * (int)((t + a) / speed)) / speed;
    const float o2 = (circleWidth + r) * (t + b - speed * (int)((t + b) / speed)) / speed;
    const float o3 = (circleWidth + r) * (t + c - speed * (int)((t + c) / speed)) / speed;

    window->DrawList->AddCircleFilled(ImVec2(pos.x + circleEnd - o1, bb.Min.y + r), r, bg_col);
    window->DrawList->AddCircleFilled(ImVec2(pos.x + circleEnd - o2, bb.Min.y + r), r, bg_col);
    window->DrawList->AddCircleFilled(ImVec2(pos.x + circleEnd - o3, bb.Min.y + r), r, bg_col);
    return true;
}

// Simple loading spinner for indeterminate tasks.
bool Spinner(const char* label, float radius, int thickness, const ImU32& color) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size((radius) * 2, (radius + style.FramePadding.y) * 2);

    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ItemSize(bb, style.FramePadding.y);
    if (!ItemAdd(bb, id))
        return false;

    window->DrawList->PathClear();

    int num_segments = 30;
    int start = abs(ImSin(g.Time * 1.8f) * (num_segments - 5));

    const float a_min = IM_PI * 2.0f * ((float)start) / (float)num_segments;
    const float a_max = IM_PI * 2.0f * ((float)num_segments - 3) / (float)num_segments;

    const ImVec2 centre = ImVec2(pos.x + radius, pos.y + radius + style.FramePadding.y);

    for (int i = 0; i < num_segments; i++) {
        const float a = a_min + ((float)i / (float)num_segments) * (a_max - a_min);
        window->DrawList->PathLineTo(ImVec2(centre.x + ImCos(a + g.Time * 8) * radius,
                                            centre.y + ImSin(a + g.Time * 8) * radius));
    }

    window->DrawList->PathStroke(color, false, thickness);
    return true;
}

bool ProgressCircle(const char* label, float radius, float thickness, float value,
                    const ImU32& color, const ImU32& bgColor) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size((radius) * 2, (radius + style.FramePadding.y) * 2);
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ItemSize(bb, style.FramePadding.y);
    if (!ItemAdd(bb, id))
        return false;

    ImVec2 centre = ImVec2(pos.x + radius, pos.y + radius + style.FramePadding.y);
    float startAngle = -IM_PI * 0.5f;
    float endAngle = startAngle + IM_PI * 2.0f * ImClamp(value, 0.0f, 1.0f);

    window->DrawList->AddCircle(centre, radius, bgColor, 32, thickness);
    window->DrawList->PathClear();
    window->DrawList->PathArcTo(centre, radius, startAngle, endAngle, 32);
    window->DrawList->PathStroke(color, false, thickness);
    return true;
}

} // namespace ImGui
#pragma endregion

#pragma region Package Task State
namespace {
static void ComputeCoverUV(int texW, int texH, const ImVec2& targetSize, ImVec2& uv0, ImVec2& uv1) {
    uv0 = ImVec2(0.0f, 0.0f);
    uv1 = ImVec2(1.0f, 1.0f);
    if (texW <= 0 || texH <= 0 || targetSize.x <= 0.0f || targetSize.y <= 0.0f) return;

    const float texAspect = static_cast<float>(texW) / static_cast<float>(texH);
    const float targetAspect = targetSize.x / targetSize.y;

    if (texAspect > targetAspect) {
        const float newW = static_cast<float>(texH) * targetAspect;
        const float xOffset = (static_cast<float>(texW) - newW) * 0.5f;
        uv0.x = xOffset / static_cast<float>(texW);
        uv1.x = (xOffset + newW) / static_cast<float>(texW);
    } else if (texAspect < targetAspect) {
        const float newH = static_cast<float>(texW) / targetAspect;
        const float yOffset = (static_cast<float>(texH) - newH) * 0.5f;
        uv0.y = yOffset / static_cast<float>(texH);
        uv1.y = (yOffset + newH) / static_cast<float>(texH);
    }
}

static void DrawImageCover(ImDrawList* list, ImTextureID texId, const ImVec2& min,
                           const ImVec2& max, int texW, int texH, ImU32 tint, float rounding) {
    ImVec2 uv0, uv1;
    ComputeCoverUV(texW, texH, ImVec2(max.x - min.x, max.y - min.y), uv0, uv1);
    if (rounding > 0.0f) {
        list->AddImageRounded(texId, min, max, uv0, uv1, tint, rounding);
    } else {
        list->AddImage(texId, min, max, uv0, uv1, tint);
    }
}

static void DrawBlurredImageCover(ImDrawList* list, ImTextureID texId, const ImVec2& min,
                                  const ImVec2& max, int texW, int texH, float alpha, float radius) {
    const ImVec2 offsets[] = {
        ImVec2(0.0f, 0.0f),
        ImVec2(radius, 0.0f),
        ImVec2(-radius, 0.0f),
        ImVec2(0.0f, radius),
        ImVec2(0.0f, -radius),
        ImVec2(radius * 0.7f, radius * 0.7f),
        ImVec2(-radius * 0.7f, radius * 0.7f),
        ImVec2(radius * 0.7f, -radius * 0.7f),
        ImVec2(-radius * 0.7f, -radius * 0.7f),
        ImVec2(radius * 1.8f, 0.0f),
        ImVec2(-radius * 1.8f, 0.0f),
        ImVec2(0.0f, radius * 1.8f),
        ImVec2(0.0f, -radius * 1.8f)
    };
    const float weights[] = {
        0.227027f,
        0.1945946f, 0.1945946f, 0.1945946f, 0.1945946f,
        0.1216216f, 0.1216216f, 0.1216216f, 0.1216216f,
        0.054054f, 0.054054f, 0.054054f, 0.054054f
    };
    float total = 0.0f;
    for (float w : weights) total += w;

    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        float w = weights[i] / total;
        ImU32 tint = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha * w));
        DrawImageCover(list, texId,
                       ImVec2(min.x + offsets[i].x, min.y + offsets[i].y),
                       ImVec2(max.x + offsets[i].x, max.y + offsets[i].y),
                       texW, texH, tint, 0.0f);
    }
}

static fs::path MakeRelativeIfInside(const fs::path& absolutePath, const fs::path& baseRoot) {
    if (absolutePath.empty()) return absolutePath;
    std::error_code ec;
    fs::path abs = absolutePath.is_absolute() ? absolutePath : fs::absolute(absolutePath, ec);
    if (ec) abs = absolutePath;
    fs::path rel = fs::relative(abs, baseRoot, ec);
    if (ec || rel.empty()) return abs;
    for (const auto& part : rel) {
        if (part == "..") return abs;
    }
    return rel;
}

static bool SaveScriptsConfig(const fs::path& path, const ScriptBuildConfig& config, const fs::path& projectRoot, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Failed to create config folder: " + ec.message();
        return false;
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        error = "Failed to open scripts config for writing: " + path.string();
        return false;
    }

    file << "# scripts.modu\n";
    file << "cppStandard=" << config.cppStandard << "\n";
    file << "scriptsDir=" << MakeRelativeIfInside(config.scriptsDir, projectRoot).generic_string() << "\n";
    file << "outDir=" << MakeRelativeIfInside(config.outDir, projectRoot).generic_string() << "\n";
    for (const auto& include : config.includeDirs) {
        file << "includeDir=" << MakeRelativeIfInside(include, projectRoot).generic_string() << "\n";
    }
    for (const auto& define : config.defines) {
        if (define.empty()) continue;
        file << "define=" << define << "\n";
    }
    for (const auto& lib : config.linuxLinkLibs) {
        if (lib.empty()) continue;
        file << "linux.linkLib=" << lib << "\n";
    }
    for (const auto& lib : config.windowsLinkLibs) {
        if (lib.empty()) continue;
        file << "win.linkLib=" << lib << "\n";
    }
    return true;
}

static float SmoothApproach(float current, float target, float speed, float dt) {
    const float blend = 1.0f - std::exp(-speed * dt);
    return current + (target - current) * blend;
}

static float EaseOutCubic(float t) {
    t = ImClamp(t, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static float EaseInOutCubic(float t) {
    t = ImClamp(t, 0.0f, 1.0f);
    if (t < 0.5f) {
        return 4.0f * t * t * t;
    }
    const float f = -2.0f * t + 2.0f;
    return 1.0f - (f * f * f) * 0.5f;
}

static float EaseOutBack(float t) {
    t = ImClamp(t, 0.0f, 1.0f);
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float p = t - 1.0f;
    return 1.0f + c3 * p * p * p + c1 * p * p;
}

namespace LauncherSkin {
inline ImVec4 Col(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

inline const ImVec4 kWell        = Col(18, 15, 25);
inline const ImVec4 kBackdropTop = Col(38, 33, 52);
inline const ImVec4 kBackdropBot = Col(24, 21, 34);
inline const ImVec4 kChrome      = Col(45, 39, 60);
inline const ImVec4 kPanel       = Col(30, 26, 41);
inline const ImVec4 kCard        = Col(43, 37, 58);
inline const ImVec4 kCardHover   = Col(56, 48, 76);
inline const ImVec4 kCardActive  = Col(65, 56, 88);

inline const ImVec4 kOutline     = Col(69, 61, 96, 0.90f);
inline const ImVec4 kOutlineSoft = Col(58, 51, 80, 0.70f);
inline const ImVec4 kSeam        = Col(22, 19, 31);

inline const ImVec4 kAccent      = Col(148, 115, 230);
inline const ImVec4 kAccentHi    = Col(173, 144, 246);
inline const ImVec4 kAccentDeep  = Col(104, 79, 173);
inline const ImVec4 kSelection   = Col(83, 66, 133);

inline const ImVec4 kGold        = Col(226, 168, 62);
inline const ImVec4 kGoldHi      = Col(243, 189, 82);
inline const ImVec4 kGoldDeep    = Col(196, 141, 40);
inline const ImVec4 kGoldText    = Col(28, 21, 8);

inline const ImVec4 kGood        = Col(126, 206, 150);
inline const ImVec4 kWarn        = Col(232, 182, 88);
inline const ImVec4 kBad         = Col(232, 108, 104);

inline const ImVec4 kText        = Col(228, 225, 236);
inline const ImVec4 kTextDim     = Col(163, 156, 184);
inline const ImVec4 kTextFaint   = Col(124, 118, 142);

inline ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, c.w * a); }
inline ImU32 U32(const ImVec4& c, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}
} // namespace LauncherSkin

static void LauncherShadeRect(ImDrawList* drawList,
                              const ImVec2& min,
                              const ImVec2& max,
                              const ImVec4& fill,
                              ImGuiShadeClass shadeClass,
                              ImGuiShadeState shadeState,
                              float rounding,
                              float alpha = 1.0f) {
    if (max.x <= min.x || max.y <= min.y) return;
    ImGui::ShadeRect(drawList, min, max, LauncherSkin::U32(fill, alpha),
                     shadeClass, shadeState, rounding);
}

static void LauncherGroupBox(ImDrawList* drawList,
                             const ImVec2& min,
                             const ImVec2& max,
                             const char* caption,
                             float uiScale,
                             float alpha = 1.0f,
                             const ImVec4* fillOverride = nullptr) {
    if (max.x <= min.x || max.y <= min.y) return;
    const float rounding = 6.0f * uiScale;
    LauncherShadeRect(drawList, min, max,
                      fillOverride ? *fillOverride : LauncherSkin::kPanel,
                      ImGuiShadeClass_Child, ImGuiShadeState_Normal, rounding, alpha);
    drawList->AddRect(min, max, LauncherSkin::U32(LauncherSkin::kOutlineSoft, alpha),
                      rounding, 0, 1.0f);
    if (!caption || !*caption) return;

    const float captionSize = ImGui::GetFontSize() * 0.82f;
    const ImVec2 captionPos(min.x + 12.0f * uiScale, min.y + 8.0f * uiScale);
    const float captionW = ImGui::GetFont()->CalcTextSizeA(captionSize, FLT_MAX, 0.0f, caption).x;
    drawList->AddText(ImGui::GetFont(), captionSize, captionPos,
                      LauncherSkin::U32(LauncherSkin::kTextDim, alpha), caption);
    const float ruleY = captionPos.y + captionSize * 0.5f;
    const float ruleX0 = captionPos.x + captionW + 8.0f * uiScale;
    const float ruleX1 = max.x - 12.0f * uiScale;
    if (ruleX1 > ruleX0) {
        drawList->AddLine(ImVec2(ruleX0, ruleY), ImVec2(ruleX1, ruleY),
                          LauncherSkin::U32(LauncherSkin::kSeam, alpha), 1.0f);
        drawList->AddLine(ImVec2(ruleX0, ruleY + 1.0f), ImVec2(ruleX1, ruleY + 1.0f),
                          LauncherSkin::U32(LauncherSkin::kOutlineSoft, alpha * 0.5f), 1.0f);
    }
}

static float LauncherGroupBoxHeaderHeight(float uiScale) {
    return 8.0f * uiScale + ImGui::GetFontSize() * 0.82f + 8.0f * uiScale;
}

static float LauncherBadge(ImDrawList* drawList,
                           const ImVec2& pos,
                           const char* text,
                           const ImVec4& tint,
                           float uiScale,
                           float alpha = 1.0f) {
    const float fontSize = ImGui::GetFontSize() * 0.82f;
    const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    const float padX = 8.0f * uiScale;
    const float padY = 3.0f * uiScale;
    const ImVec2 max(pos.x + textSize.x + padX * 2.0f, pos.y + textSize.y + padY * 2.0f);
    const ImVec4 fill(tint.x * 0.32f, tint.y * 0.32f, tint.z * 0.34f, 1.0f);
    drawList->AddRectFilled(pos, max, LauncherSkin::U32(fill, alpha), 4.0f * uiScale);
    drawList->AddRect(pos, max, LauncherSkin::U32(LauncherSkin::WithAlpha(tint, 0.65f), alpha),
                      4.0f * uiScale, 0, 1.0f);
    drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(pos.x + padX, pos.y + padY),
                      LauncherSkin::U32(tint, alpha), text);
    return max.x - pos.x;
}

static void LauncherDetailRow(const char* label, const std::string& value, float uiScale) {
    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kText);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(value.empty() ? "-" : value.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 5.0f * uiScale));
}

static bool LauncherRailItem(const char* id,
                             const char* label,
                             const char* subtitle,
                             bool selected,
                             int badgeCount,
                             float uiScale) {
    ImGui::PushID(id);
    const float height = (subtitle && *subtitle ? 46.0f : 32.0f) * uiScale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = std::max(48.0f * uiScale, ImGui::GetContentRegionAvail().x);
    ImGui::InvisibleButton("##rail", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    const ImVec2 max(pos.x + width, pos.y + height);

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID("##railAnim");
    float anim = storage->GetFloat(key, selected ? 1.0f : 0.0f);
    anim = SmoothApproach(anim, selected ? 1.0f : (hovered ? 0.55f : 0.0f),
                          14.0f, ImGui::GetIO().DeltaTime);
    storage->SetFloat(key, anim);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (anim > 0.004f) {
        const ImVec4 fill = selected ? LauncherSkin::kSelection : LauncherSkin::kCardHover;
        LauncherShadeRect(drawList, pos, max, fill, ImGuiShadeClass_Header,
                          selected ? ImGuiShadeState_Selected : ImGuiShadeState_Hovered,
                          5.0f * uiScale, anim);
    }
    if (anim > 0.004f) {
        const float spineH = (max.y - pos.y - 10.0f * uiScale) * anim;
        const float mid = (pos.y + max.y) * 0.5f;
        drawList->AddRectFilled(ImVec2(pos.x, mid - spineH * 0.5f),
                                ImVec2(pos.x + 3.0f * uiScale, mid + spineH * 0.5f),
                                LauncherSkin::U32(LauncherSkin::kAccentHi, anim),
                                1.5f * uiScale);
    }

    float textRight = max.x - 12.0f * uiScale;
    if (badgeCount > 0) {
        char countText[16];
        std::snprintf(countText, sizeof(countText), "%d", badgeCount);
        const float countW = ImGui::CalcTextSize(countText).x;
        const ImVec2 pillMin(max.x - 12.0f * uiScale - countW - 10.0f * uiScale,
                             pos.y + (height - ImGui::GetFontSize() - 4.0f * uiScale) * 0.5f);
        const ImVec2 pillMax(max.x - 12.0f * uiScale,
                             pillMin.y + ImGui::GetFontSize() + 4.0f * uiScale);
        drawList->AddRectFilled(pillMin, pillMax,
                                LauncherSkin::U32(LauncherSkin::kWell), 999.0f);
        drawList->AddText(ImVec2(pillMin.x + 5.0f * uiScale, pillMin.y + 2.0f * uiScale),
                          LauncherSkin::U32(LauncherSkin::kTextDim), countText);
        textRight = pillMin.x - 6.0f * uiScale;
    }

    const float textX = pos.x + 13.0f * uiScale;
    const ImVec4 labelCol = selected ? LauncherSkin::kText
                                     : (hovered ? LauncherSkin::kText : LauncherSkin::kTextDim);
    drawList->PushClipRect(ImVec2(textX, pos.y), ImVec2(std::max(textX, textRight), max.y), true);
    if (subtitle && *subtitle) {
        drawList->AddText(ImVec2(textX, pos.y + 7.0f * uiScale),
                          LauncherSkin::U32(labelCol), label);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.84f,
                          ImVec2(textX, pos.y + 25.0f * uiScale),
                          LauncherSkin::U32(LauncherSkin::kTextFaint), subtitle);
    } else {
        drawList->AddText(ImVec2(textX, pos.y + (height - ImGui::GetFontSize()) * 0.5f),
                          LauncherSkin::U32(labelCol), label);
    }
    drawList->PopClipRect();

    if (hovered && subtitle && *subtitle) {
        const float needed = std::max(
            ImGui::CalcTextSize(label).x,
            ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * 0.84f, FLT_MAX, 0.0f, subtitle).x);
        if (needed > textRight - textX) {
            ImGui::SetTooltip("%s\n%s", label, subtitle);
        }
    }

    ImGui::PopID();
    return clicked;
}

enum class LauncherButtonKind { Neutral, Primary, Accent, Danger };

static void LauncherPushButtonColors(LauncherButtonKind kind) {
    using namespace LauncherSkin;
    switch (kind) {
        case LauncherButtonKind::Primary:
            ImGui::PushStyleColor(ImGuiCol_Button, kGold);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGoldHi);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGoldDeep);
            ImGui::PushStyleColor(ImGuiCol_Text, kGoldText);
            break;
        case LauncherButtonKind::Accent:
            ImGui::PushStyleColor(ImGuiCol_Button, kAccentDeep);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHi);
            ImGui::PushStyleColor(ImGuiCol_Text, kText);
            break;
        case LauncherButtonKind::Danger:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.16f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.48f, 0.20f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.13f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, kText);
            break;
        case LauncherButtonKind::Neutral:
        default:
            ImGui::PushStyleColor(ImGuiCol_Button, kCard);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kCardHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kCardActive);
            ImGui::PushStyleColor(ImGuiCol_Text, kText);
            break;
    }
}

static void LauncherPopButtonColors() { ImGui::PopStyleColor(4); }

static bool LauncherButton(const char* label, LauncherButtonKind kind, const ImVec2& size) {
    LauncherPushButtonColors(kind);
    const bool pressed = ImGui::Button(label, size);
    LauncherPopButtonColors();
    return pressed;
}

static void LauncherSettingLabel(const char* label, const char* hint, float uiScale) {
    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kText);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (hint && *hint) {
        ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(hint);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0.0f, 3.0f * uiScale));
}

static std::string FormatBytesShort(std::uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buf;
}

struct LauncherIntroTimings {
    float fadeIn = 0.16f;
    float popIn = 0.44f;
    float hold = 0.10f;
    float drift = 0.64f;
};

struct LauncherIntroState {
    float elapsed = 0.0f;
    float introTotal = 0.0f;
    float textAlpha = 0.0f;
    float popT = 1.0f;
    float driftT = 1.0f;
    float driftEase = 1.0f;
    float contentRevealT = 1.0f;
    bool finished = true;
};

static LauncherIntroState EvaluateLauncherIntro(double now,
                                                double introStartTime,
                                                bool forceFinished,
                                                const LauncherIntroTimings& timings) {
    LauncherIntroState state;
    const float popStart = timings.fadeIn;
    const float driftStart = popStart + timings.popIn + timings.hold;
    state.introTotal = driftStart + timings.drift;
    state.elapsed = forceFinished
        ? state.introTotal
        : static_cast<float>(now - introStartTime);

    if (timings.fadeIn <= 0.0001f) {
        state.textAlpha = 1.0f;
    } else {
        state.textAlpha = ImClamp(state.elapsed / timings.fadeIn, 0.0f, 1.0f);
    }

    state.popT = timings.popIn <= 0.0001f
        ? 1.0f
        : ImClamp((state.elapsed - popStart) / timings.popIn, 0.0f, 1.0f);
    state.driftT = timings.drift <= 0.0001f
        ? 1.0f
        : ImClamp((state.elapsed - driftStart) / timings.drift, 0.0f, 1.0f);
    state.driftEase = EaseInOutCubic(state.driftT);
    const float contentWindow = std::max(0.001f, timings.drift + timings.hold);
    state.contentRevealT = ImClamp((state.elapsed - (driftStart - timings.hold * 0.55f)) / contentWindow, 0.0f, 1.0f);
    state.finished = forceFinished || state.driftT >= 1.0f;
    return state;
}

struct LauncherPackageSnapshot {
    std::string fingerprint;
    std::string sourceProjectName;
    fs::path sourceProjectRoot;
    std::vector<std::string> packageLabels;
    size_t externalCount = 0;
    bool hasSource = false;
};

static std::string TrimCopy(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static std::vector<std::string> SplitString(const std::string& input, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(input);
    std::string token;
    while (std::getline(ss, token, delim)) {
        out.push_back(token);
    }
    return out;
}

enum class ProjectSettingsVisibilityMode {
    Simple = 0,
    Advanced = 1,
    Developer = 2
};

static const char* ProjectSettingsModeHelp(ProjectSettingsVisibilityMode mode) {
    switch (mode) {
        case ProjectSettingsVisibilityMode::Advanced:
            return "Advanced shows technical settings most engine users may need.";
        case ProjectSettingsVisibilityMode::Developer:
            return "Developer shows debug, internal, and experimental controls.";
        case ProjectSettingsVisibilityMode::Simple:
        default:
            return "Simple shows common safe settings and hides noisy options.";
    }
}

static bool IsSettingVisibleForMode(ProjectSettingsVisibilityMode current,
                                    ProjectSettingsVisibilityMode required) {
    return static_cast<int>(current) >= static_cast<int>(required);
}

static std::string ProjectSettingsLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool ProjectSettingsMatchesSearch(const char* query,
                                         const char* section,
                                         const char* label,
                                         const char* tags = nullptr) {
    if (!query || !*query) {
        return true;
    }

    const std::string needle = ProjectSettingsLowerCopy(TrimCopy(query));
    if (needle.empty()) {
        return true;
    }

    std::string haystack;
    if (section) haystack += section;
    haystack += ' ';
    if (label) haystack += label;
    haystack += ' ';
    if (tags) haystack += tags;

    return ProjectSettingsLowerCopy(haystack).find(needle) != std::string::npos;
}

static bool IsValidProjectNameToken(const std::string& value) {
    if (TrimCopy(value).empty()) return false;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == ' ' || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}

static bool HasDuplicateProjectNameToken(const std::vector<std::string>& values,
                                         const std::string& candidate,
                                         size_t selfIndex) {
    const std::string lowered = ProjectSettingsLowerCopy(TrimCopy(candidate));
    if (lowered.empty()) return false;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i == selfIndex) continue;
        if (ProjectSettingsLowerCopy(TrimCopy(values[i])) == lowered) return true;
    }
    return false;
}

static void DrawHelpText(const char* text) {
    if (!text || !*text) {
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextColored(ImVec4(0.52f, 0.59f, 0.67f, 1.0f), "%s", text);
    ImGui::PopTextWrapPos();
}

static void DrawHelpTooltip(const char* text) {
    if (!text || !*text) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

struct ProjectSettingsColumnFlowState {
    bool tableOpen = false;
    bool twoColumns = false;
    bool inSecondColumn = false;
    int sectionIndex = 0;
    int splitIndex = 0;
    float sectionStartY = 0.0f;
    std::vector<float> frameHeights;
    std::vector<float>* storedHeights = nullptr;
};
static ProjectSettingsColumnFlowState g_projectSettingsColumnFlow;

static constexpr float kProjectSettingsTwoColumnMinWidth = 900.0f;

static int PickBalancedColumnSplit(const std::vector<float>& heights) {
    const int count = static_cast<int>(heights.size());
    float total = 0.0f;
    for (float h : heights) total += h;
    int bestSplit = (count + 1) / 2;
    float bestTallest = FLT_MAX;
    float leftSum = 0.0f;
    for (int split = 1; split < count; ++split) {
        leftSum += heights[split - 1];
        const float tallest = std::max(leftSum, total - leftSum);
        if (tallest < bestTallest) {
            bestTallest = tallest;
            bestSplit = split;
        }
    }
    return bestSplit;
}

static void BeginProjectSettingsColumns(std::vector<float>& sectionHeights) {
    ProjectSettingsColumnFlowState& flow = g_projectSettingsColumnFlow;
    flow = ProjectSettingsColumnFlowState{};
    flow.twoColumns = sectionHeights.size() >= 2 &&
                      ImGui::GetContentRegionAvail().x >= kProjectSettingsTwoColumnMinWidth;
    if (flow.twoColumns) {
        flow.splitIndex = PickBalancedColumnSplit(sectionHeights);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 2.0f));
    flow.tableOpen = ImGui::BeginTable("##ProjectSettingsColumns", flow.twoColumns ? 2 : 1,
                                       ImGuiTableFlags_SizingStretchSame |
                                       ImGuiTableFlags_NoPadOuterX |
                                       ImGuiTableFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!flow.tableOpen) {
        flow = ProjectSettingsColumnFlowState{};
        return;
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    flow.storedHeights = &sectionHeights;
    flow.frameHeights.swap(sectionHeights);
    flow.frameHeights.clear();
}

static void ProjectSettingsColumnsOnSectionHeader() {
    ProjectSettingsColumnFlowState& flow = g_projectSettingsColumnFlow;
    if (!flow.storedHeights) {
        return;
    }
    if (flow.sectionIndex > 0) {
        flow.frameHeights.push_back(std::max(0.0f, ImGui::GetCursorPosY() - flow.sectionStartY));
    }
    if (flow.twoColumns && !flow.inSecondColumn && flow.sectionIndex >= flow.splitIndex) {
        ImGui::TableNextColumn();
        flow.inSecondColumn = true;
    }
    flow.sectionStartY = ImGui::GetCursorPosY();
    ++flow.sectionIndex;
}

static void EndProjectSettingsColumns() {
    ProjectSettingsColumnFlowState& flow = g_projectSettingsColumnFlow;
    if (!flow.storedHeights) {
        flow = ProjectSettingsColumnFlowState{};
        return;
    }
    if (flow.sectionIndex > 0) {
        flow.frameHeights.push_back(std::max(0.0f, ImGui::GetCursorPosY() - flow.sectionStartY));
    }
    ImGui::EndTable();
    *flow.storedHeights = std::move(flow.frameHeights);
    flow = ProjectSettingsColumnFlowState{};
}

static bool DrawSettingSection(const char* icon,
                               const char* title,
                               const char* helper,
                               bool defaultOpen = true) {
    ProjectSettingsColumnsOnSectionHeader();
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.13f, 0.17f, 0.24f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.23f, 0.32f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.20f, 0.28f, 0.40f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));

    std::string label;
    if (icon && *icon) {
        label += icon;
        label += "  ";
    }
    label += title ? title : "";

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (defaultOpen) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    const bool open = ImGui::CollapsingHeader(label.c_str(), flags);
    if (helper && *helper && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(helper);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return open;
}

static bool DrawSettingSection(ImTextureID iconTexture, bool iconFlipY,
                               const char* fallbackIcon,
                               const char* title,
                               const char* helper,
                               bool defaultOpen = true) {
    ProjectSettingsColumnsOnSectionHeader();
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.13f, 0.17f, 0.24f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.23f, 0.32f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.20f, 0.28f, 0.40f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));

    const bool hasImage = iconTexture != static_cast<ImTextureID>(0);
    const float iconSize = ImGui::GetFontSize() + 2.0f;
    std::string label;
    if (hasImage) {
        const float spaceWidth = std::max(1.0f, ImGui::CalcTextSize(" ").x);
        label.assign(static_cast<size_t>(std::ceil((iconSize + 6.0f) / spaceWidth)), ' ');
    } else if (fallbackIcon && *fallbackIcon) {
        label += fallbackIcon;
        label += "  ";
    }
    label += title ? title : "";
    label += "###";
    label += title ? title : "";

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (defaultOpen) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    const bool open = ImGui::CollapsingHeader(label.c_str(), flags);
    if (hasImage) {
        const ImVec2 rectMin = ImGui::GetItemRectMin();
        const ImVec2 rectMax = ImGui::GetItemRectMax();
        const float textStartX = rectMin.x + ImGui::GetFontSize() + 6.0f * 3.0f;
        const float iconY = rectMin.y + ((rectMax.y - rectMin.y) - iconSize) * 0.5f;
        const ImVec2 uvMin = iconFlipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        const ImVec2 uvMax = iconFlipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        ImGui::GetWindowDrawList()->AddImage(iconTexture,
                                             ImVec2(textStartX, iconY),
                                             ImVec2(textStartX + iconSize, iconY + iconSize),
                                             uvMin, uvMax);
    }
    if (helper && *helper && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(helper);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return open;
}

static bool DrawSettingRow(const char* label,
                           const char* helper,
                           const std::function<bool()>& drawControl,
                           std::source_location idLocation = std::source_location::current()) {
    bool changed = false;
    ImGui::PushID(idLocation.file_name());
    ImGui::PushID(static_cast<int>(idLocation.line()));
    ImGui::PushID(static_cast<int>(idLocation.column()));
    ImGui::PushID(label ? label : "");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 1.0f));
    if (ImGui::BeginTable("##SettingRow", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 0.66f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(0.86f, 0.90f, 0.96f, 1.0f), "%s", label ? label : "");
        DrawHelpTooltip(helper);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        changed = drawControl ? drawControl() : false;
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    return changed;
}

/*static bool DrawRecommendedResetButton(const std::function<void()>& resetAction) {
    if (ImGui::Button("Reset to Recommended")) {
        if (resetAction) {
            resetAction();
        }
        return true;
    }
    return false;
}*/

static fs::path ResolveRecentProjectRoot(const std::string& recentPath) {
    fs::path path(recentPath);
    if (path.empty()) return {};
    if (path.extension() == ".modu") {
        return path.parent_path();
    }
    if (fs::is_directory(path)) {
        return path;
    }
    if (path.has_parent_path()) {
        return path.parent_path();
    }
    return path;
}

static bool ParsePackageManifestLabels(const fs::path& manifestPath,
                                       std::vector<std::string>& outLabels,
                                       size_t& outExternalCount) {
    outLabels.clear();
    outExternalCount = 0;

    std::ifstream file(manifestPath);
    if (!file.is_open()) {
        return false;
    }

    std::unordered_set<std::string> seen;
    std::string line;
    while (std::getline(file, line)) {
        const std::string cleaned = TrimCopy(line);
        if (cleaned.empty() || cleaned[0] == '#') continue;

        if (cleaned.rfind("package=", 0) == 0) {
            const std::string id = TrimCopy(cleaned.substr(8));
            if (!id.empty() && seen.insert(id).second) {
                outLabels.push_back(id);
            }
            continue;
        }

        std::string sourceTag;
        std::string sourceSuffix;
        size_t sourcePrefixLen = 0;
        if (cleaned.rfind("git=", 0) == 0) {
            sourceTag = "git";
            sourceSuffix = " (Git)";
            sourcePrefixLen = 4;
        } else if (cleaned.rfind("modupak=", 0) == 0) {
            sourceTag = "modupak";
            sourceSuffix = " (.modupak)";
            sourcePrefixLen = 8;
        } else {
            continue;
        }

        const auto parts = SplitString(cleaned.substr(sourcePrefixLen), '|');
        if (parts.empty()) continue;

        const std::string id = TrimCopy(parts[0]);
        std::string label = id;
        if (parts.size() > 1) {
            const std::string parsedName = TrimCopy(parts[1]);
            if (!parsedName.empty()) {
                label = parsedName;
            }
        }

        if (label.empty()) continue;
        const std::string key = sourceTag + ":" + id;
        if (seen.insert(key).second) {
            outLabels.push_back(label + sourceSuffix);
            outExternalCount++;
        }
    }

    return true;
}

static std::string BuildLauncherSnapshotFingerprint(const ProjectManager& manager) {
    std::ostringstream oss;
    const size_t count = std::min<size_t>(manager.recentProjects.size(), 16);
    oss << count << "|";
    for (size_t i = 0; i < count; ++i) {
        const auto& rp = manager.recentProjects[i];
        oss << rp.name << "|" << rp.path << "|" << rp.lastOpened << ";";
    }
    return oss.str();
}

static const LauncherPackageSnapshot& GetLauncherPackageSnapshot(const ProjectManager& manager, bool forceRefresh = false) {
    static LauncherPackageSnapshot cache;

    const std::string fingerprint = BuildLauncherSnapshotFingerprint(manager);
    if (!forceRefresh && cache.fingerprint == fingerprint) {
        return cache;
    }

    cache = LauncherPackageSnapshot{};
    cache.fingerprint = fingerprint;

    for (const auto& rp : manager.recentProjects) {
        const fs::path projectRoot = ResolveRecentProjectRoot(rp.path);
        if (projectRoot.empty()) continue;

        const fs::path manifestPath = projectRoot / "packages.modu";
        if (!fs::exists(manifestPath)) continue;

        std::vector<std::string> labels;
        size_t externalCount = 0;
        if (!ParsePackageManifestLabels(manifestPath, labels, externalCount)) {
            continue;
        }

        cache.hasSource = true;
        cache.sourceProjectRoot = projectRoot;
        cache.sourceProjectName = rp.name.empty() ? projectRoot.filename().string() : rp.name;
        cache.packageLabels = std::move(labels);
        cache.externalCount = externalCount;
        break;
    }

    return cache;
}

struct LauncherTemplateEntry {
    std::string displayName;
    std::string description;
    std::string presetId;
    fs::path projectRoot;
    fs::path projectFile;
    fs::path previewImage;
    ProjectPipeline pipeline = ProjectPipeline::Pipeline3D;
    Modularity::GraphicsBackend rendererBackend = Modularity::GraphicsBackend::OpenGL;
    bool isBlankPreset = false;
};

static std::vector<LauncherTemplateEntry> BuiltInProjectPresets(
    ProjectPipeline emptyPipeline = ProjectPipeline::Pipeline3D,
    Modularity::GraphicsBackend emptyRenderer = Modularity::GraphicsBackend::OpenGL) {
    std::vector<LauncherTemplateEntry> presets;
    auto add = [&](const char* id, const char* name, const char* description, ProjectPipeline pipeline) {
        LauncherTemplateEntry entry;
        entry.presetId = id;
        entry.displayName = name;
        entry.description = description;
        entry.pipeline = pipeline;
        entry.rendererBackend = Modularity::GraphicsBackend::OpenGL;
        entry.isBlankPreset = true;
        presets.push_back(std::move(entry));
    };
    add("empty", "Empty", "Clean project folders with no starter scene content.", emptyPipeline);
    add("2d-game", "2D Game", "2D pipeline setup with sprite folders and a starter controller script.", ProjectPipeline::Pipeline2D);
    add("tool-app", "Tool/App", "UI-focused app setup with runtime/editor script folders and UI assets.", ProjectPipeline::Pipeline2D);
    add("runtime-only", "Runtime Only", "Minimal runtime-oriented project with no starter editor script content.", ProjectPipeline::Pipeline3D);
    presets.front().rendererBackend = emptyRenderer;
    return presets;
}

struct ProjectFileMetadata {
    std::string name;
    ProjectPipeline pipeline = ProjectPipeline::Pipeline3D;
    Modularity::GraphicsBackend rendererBackend = Modularity::GraphicsBackend::OpenGL;
    bool found = false;
};

struct LauncherRecentProjectDisplayEntry {
    RecentProject recent;
    fs::path projectRoot;
    ProjectFileMetadata metadata;
};

static fs::path GetTemplateProjectsRoot() {
    std::error_code ec;
    fs::path root = fs::current_path() / "Template-Projects";
    fs::create_directories(root, ec);
    return root;
}

static fs::path FindTemplateProjectFile(const fs::path& candidateRoot) {
    const fs::path direct = candidateRoot / "project.modu";
    if (fs::exists(direct)) {
        return direct;
    }

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(candidateRoot, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        if (it->path().filename() == "project.modu") {
            return it->path();
        }
    }
    return {};
}

static ProjectFileMetadata ReadProjectFileMetadata(const fs::path& projectFile) {
    ProjectFileMetadata metadata;
    std::ifstream file(projectFile);
    if (!file.is_open()) {
        return metadata;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("name=", 0) == 0) {
            metadata.name = TrimCopy(line.substr(5));
        } else if (line.rfind("pipeline=", 0) == 0) {
            metadata.pipeline = ParseProjectPipeline(TrimCopy(line.substr(9)));
            metadata.found = true;
        } else if (line.rfind("renderer=", 0) == 0) {
            metadata.rendererBackend = Modularity::GraphicsBackendFromString(TrimCopy(line.substr(9)));
            metadata.found = true;
        }
    }

    metadata.found = metadata.found || !metadata.name.empty();
    return metadata;
}

struct LauncherProjectStats {
    std::uintmax_t sizeBytes = 0;
    int sceneCount = 0;
    int scriptCount = 0;
    int packageCount = 0;
    bool truncated = false;
    bool valid = false;
};

static const LauncherProjectStats& GetLauncherProjectStats(const fs::path& projectRoot) {
    static std::unordered_map<std::string, LauncherProjectStats> cache;
    static const LauncherProjectStats kEmpty;
    if (projectRoot.empty()) return kEmpty;

    const std::string key = projectRoot.string();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    LauncherProjectStats stats;
    std::error_code ec;
    if (!fs::exists(projectRoot, ec) || !fs::is_directory(projectRoot, ec)) {
        return cache.emplace(key, stats).first->second;
    }

    auto isSkippedDir = [](const std::string& name) {
        return name == "Cache" || name == "Builds" || name == ".git" ||
               name == "Library" || name == "Temp";
    };

    const size_t kEntryBudget = 20000;
    size_t visited = 0;
    for (auto walker = fs::recursive_directory_iterator(
             projectRoot, fs::directory_options::skip_permission_denied, ec);
         walker != fs::recursive_directory_iterator(); walker.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (++visited > kEntryBudget) { stats.truncated = true; break; }

        const fs::path& path = walker->path();
        if (walker->is_directory(ec)) {
            if (isSkippedDir(path.filename().string())) {
                walker.disable_recursion_pending();
            }
            continue;
        }
        if (!walker->is_regular_file(ec)) continue;

        const std::uintmax_t size = walker->file_size(ec);
        if (!ec) stats.sizeBytes += size;
        ec.clear();

        const std::string ext = path.extension().string();
        if (ext == ".scene") ++stats.sceneCount;
        else if (ext == ".moducpp" || ext == ".cs") ++stats.scriptCount;
    }

    std::ifstream manifest(projectRoot / "packages.modu");
    if (manifest.is_open()) {
        std::string line;
        while (std::getline(manifest, line)) {
            const std::string cleaned = TrimCopy(line);
            if (cleaned.empty() || cleaned[0] == '#') continue;
            if (cleaned.rfind("package=", 0) == 0 || cleaned.rfind("git=", 0) == 0 ||
                cleaned.rfind("modupak=", 0) == 0) {
                ++stats.packageCount;
            }
        }
    }

    stats.valid = true;
    return cache.emplace(key, stats).first->second;
}

static std::string BuildLauncherRecentProjectsFingerprint(const ProjectManager& manager) {
    std::ostringstream oss;
    oss << manager.recentProjects.size() << "|";
    for (const auto& rp : manager.recentProjects) {
        const fs::path projectFile = ResolveRecentProjectRoot(rp.path) / "project.modu";
        std::error_code ec;
        const long long writeTime = static_cast<long long>(
            fs::exists(projectFile, ec) && !ec
                ? fs::last_write_time(projectFile, ec).time_since_epoch().count()
                : 0);
        oss << rp.name << "|" << rp.path << "|" << rp.lastOpened << "|" << writeTime << ";";
    }
    return oss.str();
}

static const std::vector<LauncherRecentProjectDisplayEntry>& GetLauncherRecentProjectDisplayEntries(const ProjectManager& manager,
                                                                                                    bool forceRefresh = false) {
    static std::string cachedFingerprint;
    static std::vector<LauncherRecentProjectDisplayEntry> cachedEntries;

    const std::string fingerprint = BuildLauncherRecentProjectsFingerprint(manager);
    if (!forceRefresh && fingerprint == cachedFingerprint) {
        return cachedEntries;
    }

    cachedFingerprint = fingerprint;
    cachedEntries.clear();
    cachedEntries.reserve(manager.recentProjects.size());

    for (const RecentProject& rp : manager.recentProjects) {
        LauncherRecentProjectDisplayEntry entry;
        entry.recent = rp;
        entry.projectRoot = ResolveRecentProjectRoot(rp.path);
        const fs::path projectFile = entry.projectRoot / "project.modu";
        if (fs::exists(projectFile)) {
            entry.metadata = ReadProjectFileMetadata(projectFile);
        }
        cachedEntries.push_back(std::move(entry));
    }

    return cachedEntries;
}

static std::vector<LauncherTemplateEntry> GatherTemplateEntries() {
    std::vector<LauncherTemplateEntry> templates;
    const fs::path templatesRoot = GetTemplateProjectsRoot();
    if (!fs::exists(templatesRoot)) {
        return templates;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(templatesRoot, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;

        fs::path projectFile = FindTemplateProjectFile(entry.path());
        if (projectFile.empty()) continue;

        LauncherTemplateEntry t;
        t.projectFile = projectFile;
        t.projectRoot = projectFile.parent_path();
        t.displayName = entry.path().filename().string();
        t.previewImage = t.projectRoot / "ProjectUserSettings" / "ProjectPreview.png";
        const ProjectFileMetadata metadata = ReadProjectFileMetadata(projectFile);
        t.pipeline = metadata.pipeline;
        t.rendererBackend = metadata.rendererBackend;
        templates.push_back(std::move(t));
    }

    std::sort(templates.begin(), templates.end(),
              [](const LauncherTemplateEntry& a, const LauncherTemplateEntry& b) {
                  return a.displayName < b.displayName;
              });
    return templates;
}

constexpr const char* kModularityTermsVersion = "modularity-tos-v1";

constexpr const char* kModularityTermsText = R"(Modularity Engine Terms of Service

Copyright (c) 2025-2026
Shock Interactive LLC and Tareno Labs LLC

By using the Modularity Engine, editor, runtime, tools, or official components, you agree to these Terms of Service.

1. Definitions
- Engine: the Modularity engine source code, core runtime, editor, tools, and official components distributed as part of the Modularity project.
- Software Built Using the Engine: any video game, application, service, tool, or other software created using the Engine.
- Marketplace Content: assets, plugins, ModuPaks, extensions, templates, or other packages intended for use with the Engine.

2. Permission to Use
You may use, copy, modify, merge, publish, and distribute software built using the Engine, free of charge, subject to these terms.

3. Commercial Use
You may use the Engine to develop commercial or closed-source software.

You may:
- Sell video games, applications, or services built using the Engine.
- Distribute commercial software built using the Engine.
- Keep the source code of software built using the Engine private.

Software built using the Engine is not required to be open source.

4. Marketplace Content
You may create and distribute Marketplace Content for use with the Engine.

Marketplace Content:
- May be distributed commercially or free of charge.
- May be licensed under any license chosen by its creator, including closed-source licenses.
- Does not automatically become part of the Engine.

Creators retain ownership and licensing control over their Marketplace Content.

5. Modifications to the Engine
You may modify the Engine for personal, research, or internal use without publishing those modifications.

If you distribute the Engine or a modified version of the Engine:
- The full corresponding source code of the modified Engine must be released under these same terms.
- The source code must be made available in a publicly accessible location without unreasonable access restrictions.
- Distributed versions must include a clear notice describing the modifications made.
- All original copyright notices must be retained.

These requirements apply only to the Engine itself, not to software built using the Engine.

6. Distribution of the Engine
You may distribute the Engine in original or modified form only under these terms.

You may not:
- Sell the Engine itself as a standalone commercial engine product.
- Sublicense the Engine under a different license.
- Rebrand the Engine and present it as a different engine.

Forks or modified versions must clearly acknowledge that they are based on the Modularity Engine.

7. Attribution
Software distributed using the Engine must include visible attribution to Modularity in at least one of the following locations:
- Software credits
- Engine Handbook
- An About section
- A similar visible acknowledgment

Example attribution:
Powered by the Modularity Engine

8. Trademarks
The names "Modularity" and "ModuEngine" are trademarks of Shock Interactive LLC and Tareno Labs LLC.
These trademarks may not be used to imply endorsement, official status, or affiliation without explicit permission from the trademark holders.

9. Disclaimer
The Engine is provided "as is", without warranty of any kind, express or implied, including merchantability, fitness for a particular purpose, and noninfringement.

In no event shall the authors or copyright holders be liable for any claim, damages, or other liability arising from the use of the Engine.)";
} // namespace
#pragma endregion

void Engine::renderEditorLanguagePromptPopup() {
#if MODULARITY_RUNTIME_ONLY
    return;
#else
    static bool checked = false;
    static bool triggerPopup = false;
    static std::string suggestedId;

    if (!checked) {
        checked = true;
        if (!projectManager.osLanguagePromptAnswered) {
            const std::string detected = Loc::DetectOperatingSystemLanguage();
            if (!detected.empty() && detected != Loc::CurrentLanguageId()) {
                suggestedId = detected;
                triggerPopup = true;
            } else {
                projectManager.osLanguagePromptAnswered = true;
                projectManager.saveLauncherSettings();
            }
        }
    }

    if (suggestedId.empty()) return;
    if (requiresTermsOfServiceAcceptance()) return;

    if (triggerPopup) {
        ImGui::OpenPopup(Loc::WindowRef("Editor Language Prompt"));
        triggerPopup = false;
    }
    if (!ImGui::BeginPopupModal(Loc::Window("DIALOG_OS_LANGUAGE_TITLE", "Editor Language Prompt"),
                                nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const Modularity::Loc::LanguageInfo* suggested = Loc::FindLanguage(suggestedId);
    const std::string suggestedName = suggested ? suggested->displayName : suggestedId;
    const std::string currentName = Loc::CurrentLanguage().displayName;

    ImGui::TextWrapped(Loc::T("DIALOG_OS_LANGUAGE_BODY",
                              "Your system language is %s. Modularity is currently in %s."),
                       suggestedName.c_str(), currentName.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped(Loc::T("DIALOG_OS_LANGUAGE_QUESTION", "Switch the editor to %s?"),
                       suggestedName.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Loc::T("DIALOG_OS_LANGUAGE_NOTE",
                                     "You can change this any time in Project Settings > Language Manager."));
    ImGui::Spacing();

    auto answer = [&](bool switchLanguage) {
        if (switchLanguage) {
            projectManager.editorLanguage = suggestedId;
            Loc::SetLanguage(suggestedId);
        }
        projectManager.osLanguagePromptAnswered = true;
        projectManager.saveLauncherSettings();
        suggestedId.clear();
        ImGui::CloseCurrentPopup();
    };

    if (ImGui::Button(Loc::T("DIALOG_OS_LANGUAGE_SWITCH", "Switch"), ImVec2(150, 0))) {
        answer(true);
    }
    ImGui::SameLine();
    if (ImGui::Button(Loc::T("DIALOG_OS_LANGUAGE_KEEP_ENGLISH", "Keep English"), ImVec2(150, 0))) {
        answer(false);
    }
    ImGui::EndPopup();
#endif
}

bool Engine::requiresTermsOfServiceAcceptance() const {
#ifdef MODULARITY_PLAYER
    return false;
#else
    return projectManager.acceptedTermsVersion != kModularityTermsVersion;
#endif
}

void Engine::renderTermsOfServiceModal() {
#ifdef MODULARITY_PLAYER
    return;
#else
    if (!requiresTermsOfServiceAcceptance()) {
        termsPopupOpened = false;
        return;
    }
    if (showLauncher && !launcherIntroFinished) {
        return;
    }

    if (!termsPopupOpened) {
        ImGui::OpenPopup("Modularity Terms of Service");
        termsPopupOpened = true;
    }

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const ImVec2 popupSize(
        ImClamp(displaySize.x * 0.72f, 640.0f, 920.0f),
        ImClamp(displaySize.y * 0.80f, 520.0f, 760.0f));
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(popupSize, ImGuiCond_Appearing);

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::BeginPopupModal("Modularity Terms of Service", nullptr, popupFlags)) {
        auto centeredText = [](const char* text, const ImVec4& color) {
            const ImVec2 textSize = ImGui::CalcTextSize(text);
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > textSize.x) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - textSize.x) * 0.5f);
            }
            ImGui::TextColored(color, "%s", text);
        };

        auto beginCenteredColumn = [](float maxWidth) {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float width = ImMax(120.0f, ImMin(maxWidth, avail));
            if (avail > width) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) * 0.5f);
            }
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + width);
            return width;
        };

        auto endCenteredColumn = []() {
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
        };

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.12f, 0.16f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.30f, 0.40f, 0.95f));

        centeredText("Modularity Engine Terms of Service", ImVec4(0.92f, 0.96f, 1.00f, 1.0f));
        ImGui::Spacing();
        beginCenteredColumn(520.0f);
        ImGui::TextWrapped("Please review these terms before using Modularity. Accepting records this version for this installation so you only see it again if the terms change.");
        endCenteredColumn();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 3.6f;
        if (ImGui::BeginChild("TermsOfServiceScroll", ImVec2(0.0f, -footerHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            beginCenteredColumn(720.0f);

            auto renderWrappedParagraph = [](const std::string& text, const ImVec4* color = nullptr) {
                if (color) {
                    ImGui::PushStyleColor(ImGuiCol_Text, *color);
                }
                ImGui::TextWrapped("%s", text.c_str());
                if (color) {
                    ImGui::PopStyleColor();
                }
            };

            auto renderWrappedBullet = [](const std::string& text) {
                const float bulletStartX = ImGui::GetCursorPosX();
                ImGui::Bullet();
                const float textStartX = ImGui::GetCursorPosX();
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::PushTextWrapPos(textStartX + ImGui::GetContentRegionAvail().x);
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopTextWrapPos();

                const float lineHeight = ImGui::GetTextLineHeight();
                if (ImGui::GetCursorPosX() < bulletStartX) {
                    ImGui::SetCursorPosX(bulletStartX);
                }
                if (ImGui::GetTextLineHeightWithSpacing() > lineHeight) {
                    ImGui::Spacing();
                }
            };

            std::istringstream termsStream(kModularityTermsText);
            std::string line;
            while (std::getline(termsStream, line)) {
                if (line.empty()) {
                    ImGui::Spacing();
                    continue;
                }

                if (line == "Modularity Engine Terms of Service") {
                    continue;
                }

                if (line.rfind("Copyright", 0) == 0) {
                    ImGui::TextDisabled("%s", line.c_str());
                    continue;
                }

                if (line.rfind("By using", 0) == 0) {
                    ImGui::Spacing();
                    renderWrappedParagraph(line);
                    continue;
                }

                if (std::isdigit(static_cast<unsigned char>(line[0])) && line.find('.') != std::string::npos) {
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.88f, 0.93f, 1.00f, 1.0f), "%s", line.c_str());
                    continue;
                }

                if (line.rfind("- ", 0) == 0) {
                    renderWrappedBullet(line.substr(2));
                    continue;
                }

                if (line.rfind("Example attribution:", 0) == 0) {
                    ImGui::Spacing();
                    const ImVec4 accentColor(0.70f, 0.80f, 0.96f, 1.0f);
                    renderWrappedParagraph(line, &accentColor);
                    continue;
                }

                renderWrappedParagraph(line);
            }

            endCenteredColumn();
        }
        ImGui::EndChild();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        ImGui::Spacing();
        beginCenteredColumn(520.0f);
        ImGui::TextDisabled("Version: %s", kModularityTermsVersion);
        endCenteredColumn();
        ImGui::Spacing();

        const ImGuiStyle& style = ImGui::GetStyle();
        const float declineWidth = 170.0f;
        const float acceptWidth = 190.0f;
        const float totalButtonWidth = declineWidth + style.ItemSpacing.x + acceptWidth;
        const float buttonAvail = ImGui::GetContentRegionAvail().x;
        if (buttonAvail > totalButtonWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (buttonAvail - totalButtonWidth) * 0.5f);
        }

        if (ImGui::Button("Decline and Exit", ImVec2(170.0f, 0.0f))) {
            glfwSetWindowShouldClose(editorWindow, GLFW_TRUE);
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.38f, 0.66f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.44f, 0.74f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.33f, 0.58f, 1.0f));
        if (ImGui::Button("Accept and Continue", ImVec2(190.0f, 0.0f))) {
            projectManager.acceptedTermsVersion = kModularityTermsVersion;
            projectManager.saveLauncherSettings();
            termsPopupOpened = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);

        ImGui::EndPopup();
    }
#endif
}

void Engine::renderWindowsDisclaimerPopup() {
#if defined(_WIN32) && !defined(MODULARITY_PLAYER)
    if (projectManager.windowsDisclaimerAcknowledgedV68) {
        windowsDisclaimerPopupOpened = false;
        return;
    }
    if (requiresTermsOfServiceAcceptance()) {
        return;
    }
    if (showLauncher && !launcherIntroFinished) {
        return;
    }

    constexpr const char* popupTitle = "Windows Disclaimer!";
    if (!windowsDisclaimerPopupOpened) {
        ImGui::OpenPopup(popupTitle);
        windowsDisclaimerPopupOpened = true;
    }

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const float popupWidth = ImClamp(displaySize.x * 0.46f, 440.0f, 620.0f);
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(popupWidth, 0.0f), ImGuiCond_Appearing);

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::BeginPopupModal(popupTitle, nullptr, popupFlags)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + popupWidth - ImGui::GetStyle().WindowPadding.x * 2.0f);
        ImGui::TextWrapped("Hey!");
        ImGui::Spacing();
        ImGui::TextWrapped("Modularity on Windows is currently not fully stable due to Windows having its own quirks and slower filesystem/toolchain behavior compared to Linux builds.");
        ImGui::Spacing();
        ImGui::TextWrapped("Because of this, you may experience:");
        ImGui::BulletText("Slower compilation times");
        ImGui::BulletText("Longer project loading times");
        ImGui::BulletText("Occasional editor instability");
        ImGui::Spacing();
        ImGui::TextWrapped("Linux builds are currently the primary recommended experience for development.");
        ImGui::Spacing();
        ImGui::TextWrapped("Thank you for your patience while Windows support continues improving!");
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        constexpr float buttonWidth = 120.0f;
        const float available = ImGui::GetContentRegionAvail().x;
        if (available > buttonWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - buttonWidth) * 0.5f);
        }
        if (ImGui::Button("Got it!", ImVec2(buttonWidth, 0.0f))) {
            projectManager.windowsDisclaimerAcknowledgedV68 = true;
            projectManager.saveLauncherSettings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#else
    windowsDisclaimerPopupOpened = false;
#endif
}

void Engine::renderLowSpecBenchmarkPopup() {
#if !MODULARITY_RUNTIME_ONLY
    namespace HP = Modularity::HardwareProfile;

    EditorGlobalPreferences& prefs = projectManager.preferences;
    // The Ctrl+T+L launcher preview shows the card on machines the detector never flags,
    // and leaves without touching the saved answer or the hardware tier.
    const bool preview = lowSpecBenchmarkPopupForced;
    if (!preview && (prefs.lowSpecPromptAnswered || !HP::LooksLowEnd())) {
        lowSpecBenchmarkPopupOpened = false;
        return;
    }
    if (requiresTermsOfServiceAcceptance()) return;
#if defined(_WIN32)
    if (!projectManager.windowsDisclaimerAcknowledgedV68) return;
#endif
    if (showLauncher && !launcherIntroFinished) return;

    constexpr const char* popupTitle = "Performance check";
    if (!lowSpecBenchmarkPopupOpened) {
        ImGui::OpenPopup(popupTitle);
        lowSpecBenchmarkPopupOpened = true;
    }

    CardModalIcon icon{};
    {
        constexpr const char* iconPath =
            "Resources/Engine-Root/Pop-up Confirmation Icons/Low End PC detection Icon.png";
        if (rendererInitialized) {
            if (Texture* tex = renderer.getTexture(iconPath, MaterialProperties::TextureFilter::Point);
                tex && tex->GetID()) {
                icon = { static_cast<ImTextureID>(tex->GetID()), true };
            }
        }
        if (icon.id == static_cast<ImTextureID>(0) && usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
            icon = { vulkanRenderer->getOrCreateUIImage(iconPath), false };
        }
    }

    // Wider than the rename/delete cards because this one carries three paragraphs.
    const float cardWidth = ImClamp(ImGui::GetIO().DisplaySize.x * 0.42f, 430.0f, 560.0f);

    auto dismiss = [&]() {
        if (preview) {
            lowSpecBenchmarkPopupForced = false;
            addConsoleMessage("Low-end prompt preview closed (no settings were changed)",
                              ConsoleMessageType::Info);
        } else {
            prefs.lowSpecPromptAnswered = true;
            projectManager.saveLauncherSettings();
        }
        lowSpecBenchmarkPopupOpened = false;
        ImGui::CloseCurrentPopup();
    };

    if (beginCardModal(popupTitle, cardWidth, nullptr, icon)) {
        cardModalText("Modularity has detected a low-end system.");
        cardModalText(HP::Static().summary().c_str());
        cardModalText("Would you like to run a microbenchmark and auto profile for "
                      "performance tweaks? It renders a few test frames offscreen, "
                      "takes about a second, and nothing changes until it finishes.");

        static constexpr CardModalAction actions[] = {
            { "Sure!", CardButtonKind::Primary, false },
            { "Nah, I'll manually tweak it", CardButtonKind::Neutral, false },
        };
        const int answer = cardModalActions(actions, IM_ARRAYSIZE(actions));
        if (answer == 0) {
            if (!preview) {
                const HP::BenchmarkResult r = HP::RunBenchmark();
                if (r.valid) {
                    prefs.hardwareTier = HP::ToString(r.tier);
                    prefs.benchFullscreenPassMs = r.fullscreenPassMs;
                    prefs.benchBlurPassMs = r.blurPassMs;
                    prefs.benchDrawCallBatchMs = r.drawCallBatchMs;
                    syncGlassBlurToHardwareTier();
                }
            }
            dismiss();
        } else if (answer == 1) {
            dismiss();
        }

        ImGui::Spacing();
        cardModalText("Either way you can change this later in the launcher's "
                      "Settings > Performance tab.");

        endCardModal();
    }
#else
    lowSpecBenchmarkPopupOpened = false;
#endif
}

void Engine::renderAndroidStorageAccessPopup() {
#if defined(__ANDROID__) && !defined(MODULARITY_PLAYER)
    const bool hasAccess = Modularity::AndroidRuntime::HasAllFilesAccess();

    if (hasAccess) {
        const std::string docs = Modularity::AndroidRuntime::GetExternalDocumentsPath();
        const char* dataPath = Modularity::AndroidRuntime::GetInternalDataPath();
        const bool defaultIsPrivate = dataPath && dataPath[0] != '\0' &&
            std::strncmp(projectManager.defaultProjectLocation, dataPath, std::strlen(dataPath)) == 0;
        if (!docs.empty() && defaultIsPrivate) {
            const std::string docsProjects = (fs::path(docs) / "ModularityProjects").string();
            std::error_code ec;
            fs::create_directories(docsProjects, ec);
            if (!ec) {
                std::snprintf(projectManager.defaultProjectLocation,
                              sizeof(projectManager.defaultProjectLocation),
                              "%s", docsProjects.c_str());
                std::snprintf(projectManager.newProjectLocation,
                              sizeof(projectManager.newProjectLocation),
                              "%s", projectManager.defaultProjectLocation);
                projectManager.saveLauncherSettings();
            }
        }
    }

    if (hasAccess || projectManager.androidStoragePromptDismissedV1) {
        androidStorageAccessPopupOpened = false;
        return;
    }
    if (requiresTermsOfServiceAcceptance()) {
        return;
    }
    if (showLauncher && !launcherIntroFinished) {
        return;
    }

    constexpr const char* popupTitle = "Allow File Access";
    if (!androidStorageAccessPopupOpened) {
        ImGui::OpenPopup(popupTitle);
        androidStorageAccessPopupOpened = true;
    }

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const float popupWidth = ImClamp(displaySize.x * 0.46f, 440.0f, 620.0f);
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(popupWidth, 0.0f), ImGuiCond_Appearing);

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::BeginPopupModal(popupTitle, nullptr, popupFlags)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + popupWidth - ImGui::GetStyle().WindowPadding.x * 2.0f);
        ImGui::TextWrapped("Hey!");
        ImGui::Spacing();
        ImGui::TextWrapped("Modularity would like access to your device storage so it "
                           "can keep your projects in Documents/ModularityProjects, just "
                           "like on desktop.");
        ImGui::Spacing();
        ImGui::TextWrapped("Without it, projects are stored in private app storage that "
                           "other apps and your file manager can't reach, and they're "
                           "wiped if you uninstall Modularity.");
        ImGui::Spacing();
        ImGui::TextWrapped("You can change this anytime under Settings -> File Access.");
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float buttonWidth = 150.0f * uiDpiScale;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float groupWidth = buttonWidth * 2.0f + spacing;
        const float available = ImGui::GetContentRegionAvail().x;
        if (available > groupWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - groupWidth) * 0.5f);
        }
        if (ImGui::Button("Grant Access", ImVec2(buttonWidth, 0.0f))) {
            std::string reqErr;
            Modularity::AndroidRuntime::RequestAllFilesAccess(reqErr);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Not Now", ImVec2(buttonWidth, 0.0f))) {
            projectManager.androidStoragePromptDismissedV1 = true;
            projectManager.saveLauncherSettings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#else
    androidStorageAccessPopupOpened = false;
#endif
}

#pragma region Launcher
void Engine::renderLauncher() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImVec2 displaySize = mainViewport ? mainViewport->Size : io.DisplaySize;
    ImVec2 displayPos = mainViewport ? mainViewport->Pos : ImVec2(0.0f, 0.0f);
    const double now = glfwGetTime();
    const bool introEnabled = projectManager.preferences.launcherIntroAnimation;
    if (!launcherIntroStarted) {
        launcherIntroStarted = true;
        launcherIntroStartTime = now;
        if (!introEnabled) {
            launcherIntroFinished = true;
            launcherIntroSoundPlayed = true;
        }
    }
    if (!launcherIntroSoundPlayed) {
        // One-shot so that any other UI sound (like the card click) can layer on top
        // instead of cutting the intro chime off mid-play.
        playEditorFeedbackOneShot("Resources/Sounds/ModuIntro.mp3", 0.85f, EditorFeedbackSoundCategory::Boot);
        launcherIntroSoundPlayed = true;
    }

    const float launcherAnimScale =
        (uiAnimationMode == UIAnimationMode::Off)    ? 0.0f
        : (uiAnimationMode == UIAnimationMode::Snappy) ? 0.45f
                                                       : 1.0f;
    LauncherIntroTimings introTimings{};
    introTimings.fadeIn *= launcherAnimScale;
    introTimings.popIn  *= launcherAnimScale;
    introTimings.hold   *= launcherAnimScale;
    introTimings.drift  *= launcherAnimScale;
    LauncherIntroState introState = EvaluateLauncherIntro(now, launcherIntroStartTime, launcherIntroFinished, introTimings);
    if (!launcherIntroFinished && introState.finished) {
        launcherIntroFinished = true;
        introState = EvaluateLauncherIntro(now, launcherIntroStartTime, true, introTimings);
    }

    const float transitionDuration = std::max(0.0001f, 0.72f * launcherAnimScale);
    float transitionT = 0.0f;
    if (launcherTransitionActive) {
        transitionT = ImClamp(static_cast<float>((now - launcherTransitionStartTime) / transitionDuration), 0.0f, 1.0f);
    }
    // Smooth S-curve ease so the zoom feels like it accelerates and decelerates.
    const float transitionEase = EaseInOutCubic(transitionT);

    // Ctrl+T+L previews the low-end machine prompt. It lives here, on the project
    // selection screen only, so nobody trips it by accident while editing a scene.
    if (launcherIntroFinished && !launcherTransitionActive && !io.WantTextInput && io.KeyCtrl) {
        const bool chord = (ImGui::IsKeyDown(ImGuiKey_T) && ImGui::IsKeyPressed(ImGuiKey_L, false)) ||
                           (ImGui::IsKeyDown(ImGuiKey_L) && ImGui::IsKeyPressed(ImGuiKey_T, false));
        if (chord && !lowSpecBenchmarkPopupForced) {
            lowSpecBenchmarkPopupForced = true;
            lowSpecBenchmarkPopupOpened = false;  // let the popup re-open this frame
        }
    }

    const bool isLoadingActive = projectLoadInProgress || sceneLoadInProgress;
    float loadingScreenT = launcherTransitionActive ? transitionEase : 0.0f;
    if (!launcherTransitionActive && isLoadingActive && launcherTransitionStartTime > 0.0) {
        loadingScreenT = 1.0f;
    }
    const float transitionAlpha = ImClamp(1.0f - loadingScreenT * 1.1f, 0.0f, 1.0f);
    const float menuBuildT = launcherIntroFinished ? 1.0f : EaseOutCubic(introState.contentRevealT);
    const float introMenuScale = launcherIntroFinished ? 1.0f : ImLerp(1.04f, 1.0f, menuBuildT);
    const float uiScale = (1.0f + 0.04f * transitionEase) * introMenuScale * uiDpiScale;
    const float contentAlpha = launcherIntroFinished ? 1.0f : ImClamp(0.10f + introState.contentRevealT * 0.90f, 0.0f, 1.0f);
    const float headerStaticAlpha = launcherIntroFinished ? 1.0f : ImClamp((introState.driftT - 0.88f) / 0.12f, 0.0f, 1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9.0f * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f * uiScale);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    if (mainViewport) {
        ImGui::SetNextWindowViewport(mainViewport->ID);
    }
    ImGui::SetNextWindowPos(displayPos);
    ImGui::SetNextWindowSize(displaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking  |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("Launcher", nullptr, flags)) {
        static bool launcherStateRestored = false;
        static int launcherSection = 0;
        static int launcherSectionPrevious = 0;
        static double launcherSectionSwitchTime = 0.0;
        static char launcherSearch[160] = "";
        static float projectsPanelVisual = 0.0f;
        static float tabIndicatorVisualX = -1.0f;
        static float tabIndicatorVisualW = 0.0f;
        static std::string launcherSelectedProjectPath;
        static int launcherPipelineFilter = 0;

        if (!launcherStateRestored) {
            launcherStateRestored = true;
            if (projectManager.preferences.launcherRememberLastTab) {
                launcherSection = ImClamp(projectManager.preferences.launcherLastTab, 0, 2);
                launcherSectionPrevious = launcherSection;
            }
        }

        const float frameDt = ImGui::GetIO().DeltaTime;
        const float projectsPanelTarget = projectManager.showNewProjectDialog ? 1.0f : 0.0f;
        projectsPanelVisual = SmoothApproach(projectsPanelVisual, projectsPanelTarget, 12.0f, frameDt);
        const float sectionAnimDuration = 0.26f;
        const float sectionAnimT = ImClamp(static_cast<float>((glfwGetTime() - launcherSectionSwitchTime) / sectionAnimDuration), 0.0f, 1.0f);
        const float sectionAnimEase = EaseOutCubic(sectionAnimT);

        const bool launcherBusy = projectLoadInProgress || sceneLoadInProgress ||
                                  launcherTransitionActive || launcherTransitionPendingHide;
        const std::string engineVersion = PackageManager::currentEngineVersion();

        auto openNewProjectDialog = [&]() {
            projectManager.showNewProjectDialog = true;
            projectManager.errorMessage.clear();
            projectManager.newProjectImportLastPackages = false;
            projectManager.newProjectTemplatePath.clear();
            projectManager.newProjectTemplateName.clear();
            std::memset(projectManager.newProjectName, 0, sizeof(projectManager.newProjectName));
            projectManager.newProjectPipelineMode = 0;
            projectManager.newProjectRendererMode = 0;
            (void)GetTemplateProjectsRoot();
            if (projectManager.defaultProjectLocation[0] != '\0') {
                std::snprintf(projectManager.newProjectLocation,
                              sizeof(projectManager.newProjectLocation),
                              "%s",
                              projectManager.defaultProjectLocation);
            } else {
                const std::string fallback = (fs::current_path() / "Projects").string();
                std::snprintf(projectManager.newProjectLocation,
                              sizeof(projectManager.newProjectLocation),
                              "%s",
                              fallback.c_str());
            }
        };

        auto openProjectDialog = [&]() {
            projectManager.showOpenProjectDialog = true;
            projectManager.errorMessage.clear();
        };

        auto setLauncherSection = [&](int newSection) {
            if (launcherSection == newSection) return;
            launcherSectionPrevious = launcherSection;
            launcherSection = newSection;
            launcherSectionSwitchTime = glfwGetTime();
            playEditorFeedbackOneShot("Resources/Sounds/Selection Tick Main Editor.mp3", 0.85f,
                                      EditorFeedbackSoundCategory::Click);
            if (projectManager.preferences.launcherRememberLastTab &&
                projectManager.preferences.launcherLastTab != newSection) {
                projectManager.preferences.launcherLastTab = newSection;
                projectManager.saveLauncherSettings();
            }
        };

        auto launchRecentProject = [&](const RecentProject& rp, const ImVec2& focus) {
            if (launcherBusy) return;
            // One-shot so the click sits on top of the intro/wind sounds instead of
            // stopping them via the single preview slot.
            playEditorFeedbackOneShot("Resources/Sounds/Selection.mp3", 0.95f, EditorFeedbackSoundCategory::Click);
            launcherTransitionActive = true;
            launcherTransitionPendingHide = false;
            launcherTransitionFocus = focus;
            launcherTransitionProjectName = rp.name;
            fs::path previewPath = getProjectPreviewPath(rp.path);
            if (!previewPath.empty() && fs::exists(previewPath)) {
                launcherLoadingPreviewPath = previewPath.string();
            }
            launcherTransitionStartTime = glfwGetTime();
            OpenProjectPath(rp.path);
        };

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 windowSize = ImGui::GetWindowSize();

        ImTextureID previewImageId = static_cast<ImTextureID>(0);
        int previewImageWidth = 0;
        int previewImageHeight = 0;
        if ((projectLoadInProgress || sceneLoadInProgress || launcherTransitionActive || launcherTransitionPendingHide) &&
            !launcherLoadingPreviewPath.empty()) {
            fs::path previewPath = launcherLoadingPreviewPath;
            if (fs::exists(previewPath)) {
                if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
                    previewImageId = vulkanRenderer->getOrCreateUIImage(previewPath.string(),
                                                                       &previewImageWidth,
                                                                       &previewImageHeight);
                } else if (!usingVulkan()) {
                    if (Texture* previewTexture = renderer.getTexture(previewPath.string())) {
                        previewImageId = (ImTextureID)(intptr_t)previewTexture->GetID();
                        previewImageWidth = previewTexture->GetWidth();
                        previewImageHeight = previewTexture->GetHeight();
                    }
                }
            }
        }

        if (previewImageId != static_cast<ImTextureID>(0)) {
            DrawBlurredImageCover(drawList,
                                  previewImageId,
                                  windowPos,
                                  ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
                                  previewImageWidth,
                                  previewImageHeight,
                                  0.55f,
                                  10.0f * uiScale);
        }

        const float bgAlpha = (previewImageId != static_cast<ImTextureID>(0)) ? 0.62f : 1.0f;
        const ImVec4 bgTopLeft     = LauncherSkin::WithAlpha(LauncherSkin::kBackdropTop, bgAlpha);
        const ImVec4 bgTopRight    = LauncherSkin::WithAlpha(
            ImVec4(LauncherSkin::kBackdropTop.x * 1.10f,
                   LauncherSkin::kBackdropTop.y * 1.06f,
                   LauncherSkin::kBackdropTop.z * 1.14f, 1.0f), bgAlpha);
        const ImVec4 bgBottomRight = LauncherSkin::WithAlpha(LauncherSkin::kBackdropBot, bgAlpha);
        const ImVec4 bgBottomLeft  = LauncherSkin::WithAlpha(LauncherSkin::kBackdropBot, bgAlpha);
        drawList->AddRectFilledMultiColor(
            windowPos,
            ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
            ImGui::GetColorU32(bgTopLeft),
            ImGui::GetColorU32(bgTopRight),
            ImGui::GetColorU32(bgBottomRight),
            ImGui::GetColorU32(bgBottomLeft));
        if (previewImageId == static_cast<ImTextureID>(0)) {
            for (int band = 0; band < 3; ++band) {
                const float h = (110.0f + 90.0f * static_cast<float>(band)) * uiScale;
                const float a = 0.05f - 0.014f * static_cast<float>(band);
                drawList->AddRectFilledMultiColor(
                    windowPos,
                    ImVec2(windowPos.x + windowSize.x, windowPos.y + h),
                    LauncherSkin::U32(LauncherSkin::kAccent, a),
                    LauncherSkin::U32(LauncherSkin::kAccent, a),
                    LauncherSkin::U32(LauncherSkin::kAccent, 0.0f),
                    LauncherSkin::U32(LauncherSkin::kAccent, 0.0f));
            }
        }

        // (The transition zoom rect is drawn at the end of this Begin block so it sits
        //  ON TOP of the launcher content rather than being painted over by the cards.)

        const float headerHeight   = 62.0f * uiScale;
        const float headerPadX     = 30.0f * uiScale;
        const float headerLogoSize = 28.0f * uiScale;
        const float headerLogoGap  = 12.0f * uiScale;
        const float baseFontSize   = ImGui::GetFontSize();
        const float titleFontSize  = baseFontSize * 1.22f;
        const float tabFontSize    = baseFontSize * 1.00f;
        const float linkFontSize   = baseFontSize * 0.95f;

        const ImVec2 headerLogoPos(
            windowPos.x + headerPadX,
            windowPos.y + (headerHeight - headerLogoSize) * 0.5f);
        const ImVec2 headerTextPos(
            windowPos.x + headerPadX + headerLogoSize + headerLogoGap,
            windowPos.y + (headerHeight - titleFontSize) * 0.5f);

        ImTextureID logoTexId = static_cast<ImTextureID>(0);
        int logoTexWidth = 0;
        int logoTexHeight = 0;
        {
            const fs::path logoPath = fs::path("Resources") / "Engine-Root" / "Modu-Logo.png";
            if (fs::exists(logoPath)) {
                if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
                    logoTexId = vulkanRenderer->getOrCreateUIImage(logoPath.string(), &logoTexWidth, &logoTexHeight);
                } else if (!usingVulkan()) {
                    if (Texture* logoTexture = renderer.getTexture(logoPath.string())) {
                        logoTexId = (ImTextureID)(intptr_t)logoTexture->GetID();
                        logoTexWidth = logoTexture->GetWidth();
                        logoTexHeight = logoTexture->GetHeight();
                    }
                }
            }
        }
        const ImVec2 logoUvMin = usingVulkan() ? ImVec2(0.0f, 0.0f) : ImVec2(0.0f, 1.0f);
        const ImVec2 logoUvMax = usingVulkan() ? ImVec2(1.0f, 1.0f) : ImVec2(1.0f, 0.0f);

        const ImVec2 headerMin(windowPos.x, windowPos.y);
        const ImVec2 headerMax(windowPos.x + windowSize.x, windowPos.y + headerHeight);
        LauncherShadeRect(drawList, headerMin, headerMax, LauncherSkin::kChrome,
                          ImGuiShadeClass_MenuBar, ImGuiShadeState_Normal, 0.0f);
        drawList->AddLine(ImVec2(headerMin.x, headerMax.y - 0.5f),
                          ImVec2(headerMax.x, headerMax.y - 0.5f),
                          LauncherSkin::U32(LauncherSkin::kSeam),
                          1.0f);

        const float titleTextWidth = ImGui::GetFont()->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, "Modularity").x;
        const std::string versionChipText = "v" + engineVersion;
        const float versionChipFontSize = baseFontSize * 0.80f;
        const float versionChipTextW =
            ImGui::GetFont()->CalcTextSizeA(versionChipFontSize, FLT_MAX, 0.0f, versionChipText.c_str()).x;
        const float versionChipW = versionChipTextW + 14.0f * uiScale;
        const float versionChipX = headerTextPos.x + titleTextWidth + 12.0f * uiScale;

        if (headerStaticAlpha > 0.001f) {
            if (logoTexId != static_cast<ImTextureID>(0)) {
                drawList->AddImage(logoTexId,
                                   headerLogoPos,
                                   ImVec2(headerLogoPos.x + headerLogoSize, headerLogoPos.y + headerLogoSize),
                                   logoUvMin, logoUvMax,
                                   ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, headerStaticAlpha)));
            }
            drawList->AddText(ImGui::GetFont(), titleFontSize, headerTextPos,
                              LauncherSkin::U32(LauncherSkin::kText, headerStaticAlpha), "Modularity");
            const float chipH = versionChipFontSize + 7.0f * uiScale;
            const ImVec2 chipMin(versionChipX, windowPos.y + (headerHeight - chipH) * 0.5f);
            const ImVec2 chipMax(chipMin.x + versionChipW, chipMin.y + chipH);
            LauncherShadeRect(drawList, chipMin, chipMax, LauncherSkin::kWell,
                              ImGuiShadeClass_Frame, ImGuiShadeState_Normal,
                              4.0f * uiScale, headerStaticAlpha);
            drawList->AddText(ImGui::GetFont(), versionChipFontSize,
                              ImVec2(chipMin.x + 7.0f * uiScale, chipMin.y + 3.0f * uiScale),
                              LauncherSkin::U32(LauncherSkin::kAccentHi, headerStaticAlpha),
                              versionChipText.c_str());
        }

        struct TabDef { const char* label; int index; };
        const TabDef tabs[] = {
            {"Projects", 0},
            {"ModuPAK", 1},
            {"Settings", 2}
        };
        const int tabCount = (int)(sizeof(tabs) / sizeof(tabs[0]));

        const float tabsStartX = versionChipX + versionChipW + 30.0f * uiScale;
        const float tabHPad = 22.0f * uiScale;

        float tabWidths[8] = {0};
        float tabXs[8] = {0};
        {
            float currentX = tabsStartX;
            for (int i = 0; i < tabCount; ++i) {
                const float w = ImGui::GetFont()->CalcTextSizeA(tabFontSize, FLT_MAX, 0.0f, tabs[i].label).x + tabHPad * 2.0f;
                tabWidths[i] = w;
                tabXs[i] = currentX;
                currentX += w;
            }
        }

        // Right side link buttons
        struct LinkDef { const char* label; const char* url; };
        const LinkDef rightLinks[] = {
            {"Website", "https://moduengine.xyz"},
            {"Handbook",    "https://moduengine.xyz/docs"}
        };
        const int rightLinkCount = (int)(sizeof(rightLinks) / sizeof(rightLinks[0]));
        const float linkHPad = 14.0f * uiScale;
        const float linkSpacing = 4.0f * uiScale;
        const float exitW = ImGui::GetFont()->CalcTextSizeA(linkFontSize, FLT_MAX, 0.0f, "Exit").x + linkHPad * 2.0f;
        float linkWidths[4] = {0};
        float linksTotalW = exitW;
        for (int i = 0; i < rightLinkCount; ++i) {
            const float w = ImGui::GetFont()->CalcTextSizeA(linkFontSize, FLT_MAX, 0.0f, rightLinks[i].label).x + linkHPad * 2.0f;
            linkWidths[i] = w;
            linksTotalW += w + linkSpacing;
        }
        const float linksStartX = windowPos.x + windowSize.x - headerPadX - linksTotalW;

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, contentAlpha * transitionAlpha);

        for (int i = 0; i < tabCount; ++i) {
            const float tx = tabXs[i];
            const float tw = tabWidths[i];
            const float th = headerHeight;
            const ImVec2 tabMin(tx, windowPos.y);
            const ImVec2 tabMax(tx + tw, windowPos.y + th);

            ImGui::SetCursorScreenPos(tabMin);
            ImGui::PushID(tabs[i].index);
            ImGui::InvisibleButton("##LauncherTab", ImVec2(tw, th));
            const bool hovered = ImGui::IsItemHovered();
            const bool pressed = ImGui::IsItemClicked();
            ImGui::PopID();
            if (pressed) setLauncherSection(tabs[i].index);

            const bool selected = launcherSection == tabs[i].index;

            ImGui::PushID(tabs[i].index);
            ImGuiStorage* tabStorage = ImGui::GetStateStorage();
            const ImGuiID tabAnimKey = ImGui::GetID("##tabAnim");
            float tabAnim = tabStorage->GetFloat(tabAnimKey, selected ? 1.0f : 0.0f);
            tabAnim = SmoothApproach(tabAnim, selected ? 1.0f : (hovered ? 0.5f : 0.0f), 16.0f, frameDt);
            tabStorage->SetFloat(tabAnimKey, tabAnim);
            ImGui::PopID();

            const ImVec4 textCol = selected
                ? LauncherSkin::kText
                : (hovered ? LauncherSkin::kText : LauncherSkin::kTextDim);
            if (tabAnim > 0.004f) {
                const ImVec2 fillMin(tabMin.x + 5.0f * uiScale, tabMin.y + 9.0f * uiScale);
                const ImVec2 fillMax(tabMax.x - 5.0f * uiScale, tabMax.y - 6.0f * uiScale);
                LauncherShadeRect(drawList, fillMin, fillMax,
                                  selected ? LauncherSkin::kCard : LauncherSkin::kCardHover,
                                  selected ? ImGuiShadeClass_TabActive : ImGuiShadeClass_Tab,
                                  selected ? ImGuiShadeState_Selected : ImGuiShadeState_Hovered,
                                  6.0f * uiScale, tabAnim);
            }
            const float textW = ImGui::GetFont()->CalcTextSizeA(tabFontSize, FLT_MAX, 0.0f, tabs[i].label).x;
            const ImVec2 textPos(tx + (tw - textW) * 0.5f,
                                 windowPos.y + (th - tabFontSize) * 0.5f);
            drawList->AddText(ImGui::GetFont(), tabFontSize, textPos,
                              LauncherSkin::U32(textCol), tabs[i].label);
        }
        {
            const int activeIdx = launcherSection;
            const float targetX = tabXs[activeIdx] + 14.0f * uiScale;
            const float targetW = tabWidths[activeIdx] - 28.0f * uiScale;
            if (tabIndicatorVisualX < 0.0f) { tabIndicatorVisualX = targetX; tabIndicatorVisualW = targetW; }
            tabIndicatorVisualX = SmoothApproach(tabIndicatorVisualX, targetX, 16.0f, frameDt);
            tabIndicatorVisualW = SmoothApproach(tabIndicatorVisualW, targetW, 16.0f, frameDt);

            const float indH = 3.0f * uiScale;
            const float indY = windowPos.y + headerHeight - indH - 0.5f;
            const float indLeft  = tabIndicatorVisualX;
            const float indRight = tabIndicatorVisualX + tabIndicatorVisualW;
            const float indMid   = (indLeft + indRight) * 0.5f;

            // Dip the indicator alpha while it's mid-flight between tabs,
            const float motion = std::abs(targetX - tabIndicatorVisualX) +
                                 std::abs(targetW - tabIndicatorVisualW);
            const float motionT = ImClamp(motion / (48.0f * uiScale), 0.0f, 1.0f);
            const float breathe = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 2.0f);
            const float restAlpha = ImLerp(0.62f, 0.92f, breathe);
            const float centerAlpha = ImLerp(restAlpha, 0.40f, motionT);

            const ImU32 cCenter = LauncherSkin::U32(LauncherSkin::kAccentHi, centerAlpha);
            const ImU32 cEdge   = LauncherSkin::U32(LauncherSkin::kAccentHi, 0.0f);

            drawList->AddRectFilledMultiColor(// Left
                ImVec2(indLeft, indY),
                ImVec2(indMid,  indY + indH),
                cEdge, cCenter, cCenter, cEdge);
            drawList->AddRectFilledMultiColor(// Right
                ImVec2(indMid,   indY),
                ImVec2(indRight, indY + indH),
                cCenter, cEdge, cEdge, cCenter);
        }

        auto drawLinkButton = [&](float x, float w, const char* label) -> bool {
            const float bh = 32.0f * uiScale;
            const ImVec2 bMin(x, windowPos.y + (headerHeight - bh) * 0.5f);
            const ImVec2 bMax(x + w, bMin.y + bh);
            ImGui::SetCursorScreenPos(bMin);
            ImGui::PushID(label);
            ImGui::InvisibleButton("##HeaderLink", ImVec2(w, bh));
            const bool hovered = ImGui::IsItemHovered();
            const bool pressed = ImGui::IsItemClicked();
            ImGui::PopID();
            if (hovered) {
                LauncherShadeRect(drawList, bMin, bMax, LauncherSkin::kCardHover,
                                  ImGuiShadeClass_Button, ImGuiShadeState_Hovered,
                                  6.0f * uiScale);
            }
            const ImU32 col = LauncherSkin::U32(hovered ? LauncherSkin::kText
                                                        : LauncherSkin::kTextDim);
            const float tw = ImGui::GetFont()->CalcTextSizeA(linkFontSize, FLT_MAX, 0.0f, label).x;
            drawList->AddText(ImGui::GetFont(), linkFontSize,
                              ImVec2(x + (w - tw) * 0.5f, bMin.y + (bh - linkFontSize) * 0.5f),
                              col, label);
            return pressed;
        };

        {
            float lx = linksStartX;
            for (int i = 0; i < rightLinkCount; ++i) {
                if (drawLinkButton(lx, linkWidths[i], rightLinks[i].label)) {
                    #ifdef _WIN32
                    const std::string cmd = std::string("start ") + rightLinks[i].url;
                    system(cmd.c_str());
                    #else
                    const std::string cmd = std::string("xdg-open ") + rightLinks[i].url + " &";
                    system(cmd.c_str());
                    #endif
                }
                lx += linkWidths[i] + linkSpacing;
            }
            if (drawLinkButton(lx, exitW, "Exit")) {
                glfwSetWindowShouldClose(editorWindow, GLFW_TRUE);
            }
        }

        ImGui::PopStyleVar(); // header alpha

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, contentAlpha * transitionAlpha);
        ImGui::SetWindowFontScale(uiScale);
        if (!launcherIntroFinished) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        }

        const float statusBarHeight = 26.0f * uiScale;
        const float contentPadX = 22.0f * uiScale;
        const float contentPadY = 14.0f * uiScale;
        const ImVec2 sectionInnerPadding(0.0f, 0.0f);
        const float contentTopY = headerHeight + contentPadY;
        const float contentW    = windowSize.x - contentPadX * 2.0f;
        const float contentH    = windowSize.y - contentTopY - contentPadY - statusBarHeight;

        // Slight slide-in offset for the active tab content during intro and tab switches.
        const int sectionDirection = (launcherSection >= launcherSectionPrevious) ? 1 : -1;
        const float sectionOffsetY = (1.0f - sectionAnimEase) * 24.0f * uiScale * (float)sectionDirection;
        const float introContentOffset = launcherIntroFinished
            ? 0.0f
            : (1.0f - EaseOutCubic(introState.contentRevealT)) * 18.0f * uiScale;

        ImGui::SetCursorPos(ImVec2(contentPadX, contentTopY + sectionOffsetY + introContentOffset));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * sectionAnimEase);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, sectionInnerPadding);
        // Force transparent ChildBg for the section AND its subpanels so the rounded
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("LauncherActiveSection",
                          ImVec2(contentW, contentH),
                          false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(); // WindowPadding (only applies to the BeginChild above)

        auto toLower = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        };

        const float groupPadX = 11.0f * uiScale;
        const float groupPadY = 9.0f * uiScale;
        auto beginGroupPanel = [&](const char* id, const ImVec2& size, const char* caption) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            LauncherGroupBox(ImGui::GetWindowDrawList(), pos,
                             ImVec2(pos.x + size.x, pos.y + size.y), caption, uiScale);
            const float topInset = (caption && *caption) ? LauncherGroupBoxHeaderHeight(uiScale)
                                                         : groupPadY;
            const ImVec2 innerSize(std::max(16.0f, size.x - groupPadX * 2.0f),
                                   std::max(16.0f, size.y - topInset - groupPadY));
            ImGui::SetCursorScreenPos(ImVec2(pos.x + groupPadX, pos.y + topInset));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::BeginChild(id, innerSize, false);
            ImGui::PopStyleColor();
        };
        auto endGroupPanel = [&]() { ImGui::EndChild(); };

        auto resolveUiTexture = [&](const fs::path& path, int& outW, int& outH) -> ImTextureID {
            outW = 0;
            outH = 0;
            if (path.empty() || !fs::exists(path)) return static_cast<ImTextureID>(0);
            if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
                return vulkanRenderer->getOrCreateUIImage(path.string(), &outW, &outH);
            }
            if (!usingVulkan()) {
                if (Texture* tex = renderer.getTexture(path.string())) {
                    outW = tex->GetWidth();
                    outH = tex->GetHeight();
                    return (ImTextureID)(intptr_t)tex->GetID();
                }
            }
            return static_cast<ImTextureID>(0);
        };

        auto openInFileManager = [](const fs::path& target) {
            if (target.empty()) return;
#ifdef _WIN32
            const std::string cmd = "explorer \"" + target.string() + "\"";
#elif defined(__APPLE__)
            const std::string cmd = "open \"" + target.string() + "\" &";
#else
            const std::string cmd = "xdg-open \"" + target.string() + "\" >/dev/null 2>&1 &";
#endif
            system(cmd.c_str());
        };

        auto renderProjectsView = [&]() {
            const auto& recentEntries = GetLauncherRecentProjectDisplayEntries(projectManager);
            const std::string filter = toLower(TrimCopy(launcherSearch));
            EditorGlobalPreferences& prefs = projectManager.preferences;

            std::vector<size_t> visible;
            int pipelineCounts[4] = {0, 0, 0, 0};
            visible.reserve(recentEntries.size());
            for (size_t i = 0; i < recentEntries.size(); ++i) {
                const LauncherRecentProjectDisplayEntry& entry = recentEntries[i];
                ++pipelineCounts[0];
                int pipelineSlot = 1;
                switch (entry.metadata.pipeline) {
                    case ProjectPipeline::Pipeline25D: pipelineSlot = 2; break;
                    case ProjectPipeline::Pipeline2D:  pipelineSlot = 3; break;
                    case ProjectPipeline::Pipeline3D:
                    default:                           pipelineSlot = 1; break;
                }
                ++pipelineCounts[pipelineSlot];

                if (launcherPipelineFilter != 0 && launcherPipelineFilter != pipelineSlot) {
                    continue;
                }
                if (!filter.empty()) {
                    const std::string haystack = toLower(
                        entry.recent.name + " " + entry.recent.path + " " +
                        Modularity::ToString(entry.metadata.rendererBackend) + " " +
                        ProjectPipelineLabel(entry.metadata.pipeline));
                    if (haystack.find(filter) == std::string::npos) continue;
                }
                visible.push_back(i);
            }

            if (prefs.launcherProjectSort == 1) {
                std::stable_sort(visible.begin(), visible.end(),
                                 [&](size_t a, size_t b) {
                                     return toLower(recentEntries[a].recent.name) <
                                            toLower(recentEntries[b].recent.name);
                                 });
            } else if (prefs.launcherProjectSort == 2) {
                std::stable_sort(visible.begin(), visible.end(),
                                 [&](size_t a, size_t b) {
                                     return static_cast<int>(recentEntries[a].metadata.pipeline) <
                                            static_cast<int>(recentEntries[b].metadata.pipeline);
                                 });
            }

            const LauncherRecentProjectDisplayEntry* selectedEntry = nullptr;
            for (size_t idx : visible) {
                if (recentEntries[idx].recent.path == launcherSelectedProjectPath) {
                    selectedEntry = &recentEntries[idx];
                    break;
                }
            }
            if (!selectedEntry && !visible.empty()) {
                launcherSelectedProjectPath = recentEntries[visible.front()].recent.path;
                selectedEntry = &recentEntries[visible.front()];
            } else if (visible.empty()) {
                launcherSelectedProjectPath.clear();
            }

            const ImVec2 colOrigin = ImGui::GetCursorPos();
            const float availW = ImGui::GetContentRegionAvail().x;
            const float availH = ImGui::GetContentRegionAvail().y;
            const float railW = 224.0f * uiScale;
            const float detailW = 302.0f * uiScale;
            const float gap = 10.0f * uiScale;
            const float listMinW = 470.0f * uiScale;
            const bool showDetails = availW > (detailW + gap + listMinW);
            const bool showRail = availW > (railW + gap + detailW + gap + listMinW);
            const float listW = availW
                              - (showRail ? railW + gap : 0.0f)
                              - (showDetails ? detailW + gap : 0.0f);
            float colX = colOrigin.x;

            if (showRail) {
                const float actionsH = 186.0f * uiScale;
                const float libraryH = std::max(180.0f * uiScale, availH - actionsH - gap);

                ImGui::SetCursorPos(ImVec2(colX, colOrigin.y));
                beginGroupPanel("##ProjectsRailLibrary", ImVec2(railW, libraryH), "Library");
                struct FilterDef { const char* label; int mode; };
                const FilterDef filters[] = {
                    {"All Projects", 0}, {"3D Pipeline", 1}, {"2.5D Pipeline", 2}, {"2D Pipeline", 3}
                };
                for (const FilterDef& f : filters) {
                    if (LauncherRailItem(f.label, f.label, nullptr,
                                         launcherPipelineFilter == f.mode,
                                         pipelineCounts[f.mode], uiScale)) {
                        launcherPipelineFilter = f.mode;
                    }
                }

                ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                ImGui::TextUnformatted("Sort order");
                ImGui::PopStyleColor();
                const char* sortLabels[] = {"Recently opened", "Name (A-Z)", "Pipeline"};
                int sortMode = ImClamp(prefs.launcherProjectSort, 0, 2);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##ProjectSort", &sortMode, sortLabels, 3)) {
                    prefs.launcherProjectSort = sortMode;
                    projectManager.saveLauncherSettings();
                }

                ImGui::Dummy(ImVec2(0.0f, 10.0f * uiScale));
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                ImGui::TextUnformatted("Library");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextDim);
                ImGui::Text("%d tracked", pipelineCounts[0]);
                ImGui::Text("%zu shown", visible.size());
                ImGui::PopStyleColor();
                endGroupPanel();

                ImGui::SetCursorPos(ImVec2(colX, colOrigin.y + libraryH + gap));
                beginGroupPanel("##ProjectsRailActions", ImVec2(railW, actionsH), "Actions");
                const ImVec2 railBtn(-1.0f, 29.0f * uiScale);
                ImGui::BeginDisabled(launcherBusy);
                if (LauncherButton("New Project...", LauncherButtonKind::Primary, railBtn)) {
                    openNewProjectDialog();
                }
                if (LauncherButton("Open Existing...", LauncherButtonKind::Neutral, railBtn)) {
                    openProjectDialog();
                }
                ImGui::EndDisabled();
                if (LauncherButton("Browse Folder", LauncherButtonKind::Neutral, railBtn)) {
                    openInFileManager(fs::path(projectManager.defaultProjectLocation));
                }
                if (LauncherButton("Refresh List", LauncherButtonKind::Neutral, railBtn)) {
                    projectManager.loadRecentProjects();
                    (void)GetLauncherRecentProjectDisplayEntries(projectManager, true);
                }
                endGroupPanel();

                colX += railW + gap;
            }

            ImGui::SetCursorPos(ImVec2(colX, colOrigin.y));
            beginGroupPanel("##ProjectsListPanel", ImVec2(listW, availH), "Recent Projects");
            {
                const float searchH = 26.0f * uiScale;
                const float toolbarW = ImGui::GetContentRegionAvail().x;
                const float compactBtnW = 84.0f * uiScale;
                const bool inlineActions = !showRail && toolbarW > 360.0f * uiScale;
                const float searchW = inlineActions
                    ? toolbarW - (compactBtnW * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f)
                    : toolbarW;

                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * uiScale);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, LauncherSkin::kWell);
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, LauncherSkin::kPanel);
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, LauncherSkin::kPanel);
                ImGui::SetNextItemWidth(searchW);
                ImGui::InputTextWithHint("##ProjectSearch", "Search name, path, renderer or pipeline...",
                                         launcherSearch, sizeof(launcherSearch));
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar();

                if (inlineActions) {
                    ImGui::SameLine();
                    ImGui::BeginDisabled(launcherBusy);
                    if (LauncherButton("Open", LauncherButtonKind::Neutral,
                                       ImVec2(compactBtnW, searchH))) {
                        openProjectDialog();
                    }
                    ImGui::SameLine();
                    if (LauncherButton("New", LauncherButtonKind::Primary,
                                       ImVec2(compactBtnW, searchH))) {
                        openNewProjectDialog();
                    }
                    ImGui::EndDisabled();
                }

                ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                ImGui::Text("%zu of %d project%s  -  click to inspect, double-click or Open to launch",
                            visible.size(), pipelineCounts[0],
                            pipelineCounts[0] == 1 ? "" : "s");
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
            }

            ImGui::PushStyleColor(ImGuiCol_ChildBg, LauncherSkin::kWell);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f * uiScale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * uiScale, 6.0f * uiScale));
            ImGui::BeginChild("ProjectsCardList", ImVec2(0.0f, 0.0f), true);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();

            {
                const float listWidth = ImGui::GetContentRegionAvail().x;
                const float cardHeight = 72.0f * uiScale;
                const float cardGap    = 6.0f * uiScale;
                const float cardPadX   = 9.0f * uiScale;
                const bool showThumbs  = prefs.launcherProjectThumbnails;
                const float thumbW     = showThumbs ? 96.0f * uiScale : 0.0f;
                const float rowsBuildT = launcherIntroFinished ? 1.0f
                                                               : EaseOutCubic(introState.contentRevealT);
                ImDrawList* listDL = ImGui::GetWindowDrawList();
                bool removedProject = false;

                for (size_t v = 0; v < visible.size(); ++v) {
                    const size_t entryIndex = visible[v];
                    const LauncherRecentProjectDisplayEntry& entry = recentEntries[entryIndex];
                    const bool isSelected = entry.recent.path == launcherSelectedProjectPath;

                    const float rowDelay = std::min(0.72f, static_cast<float>(v) * 0.045f);
                    const float rowInput = ImClamp((rowsBuildT - rowDelay) /
                                                       std::max(0.0001f, 1.0f - rowDelay),
                                                   0.0f, 1.0f);
                    const float rowReveal = EaseOutCubic(rowInput);
                    const float rowSlideY = (1.0f - rowReveal) * 14.0f * uiScale;
                    const float rowAlpha  = rowReveal;

                    ImGui::PushID(static_cast<int>(entryIndex));
                    const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + rowSlideY));
                    ImGui::SetNextItemAllowOverlap();
                    ImGui::InvisibleButton("##ProjectCard", ImVec2(listWidth, cardHeight));
                    const bool hovered    = ImGui::IsItemHovered();
                    const bool clicked    = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                    const bool dblClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                    const ImVec2 rowMin = ImGui::GetItemRectMin();
                    const ImVec2 rowMax = ImGui::GetItemRectMax();
                    const ImVec2 rowCenter((rowMin.x + rowMax.x) * 0.5f, (rowMin.y + rowMax.y) * 0.5f);

                    bool shouldOpen   = dblClicked;
                    bool shouldRemove = false;
                    if (clicked) {
                        launcherSelectedProjectPath = entry.recent.path;
                    }

                    if (ImGui::BeginPopupContextItem("##CardContext")) {
                        launcherSelectedProjectPath = entry.recent.path;
                        if (ImGui::MenuItem("Open Project", nullptr, false, !launcherBusy)) shouldOpen = true;
                        if (ImGui::MenuItem("Show in File Manager")) {
                            openInFileManager(entry.projectRoot);
                        }
                        if (ImGui::MenuItem("Copy Path")) {
                            ImGui::SetClipboardText(entry.recent.path.c_str());
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Remove from Recent")) shouldRemove = true;
                        ImGui::EndPopup();
                    }

                    ImGuiStorage* rowStorage = ImGui::GetStateStorage();
                    const ImGuiID rowAnimKey = ImGui::GetID("##rowAnim");
                    float rowAnim = rowStorage->GetFloat(rowAnimKey, isSelected ? 1.0f : 0.0f);
                    rowAnim = SmoothApproach(rowAnim,
                                             isSelected ? 1.0f : (hovered ? 0.62f : 0.0f),
                                             15.0f, frameDt);
                    rowStorage->SetFloat(rowAnimKey, rowAnim);

                    const float radius = 6.0f * uiScale;
                    const ImVec4 baseFill = LauncherSkin::kCard;
                    const ImVec4 hotFill  = isSelected ? LauncherSkin::kSelection
                                                       : LauncherSkin::kCardHover;
                    LauncherShadeRect(listDL, rowMin, rowMax, baseFill,
                                      ImGuiShadeClass_Header, ImGuiShadeState_Normal,
                                      radius, rowAlpha);
                    if (rowAnim > 0.004f) {
                        LauncherShadeRect(listDL, rowMin, rowMax, hotFill,
                                          ImGuiShadeClass_Header,
                                          isSelected ? ImGuiShadeState_Selected
                                                     : ImGuiShadeState_Hovered,
                                          radius, rowAlpha * rowAnim);
                    }
                    listDL->AddRect(rowMin, rowMax,
                                    LauncherSkin::U32(isSelected ? LauncherSkin::kAccent
                                                                 : LauncherSkin::kOutlineSoft,
                                                      rowAlpha * (isSelected ? 0.85f : 0.6f)),
                                    radius, 0, 1.0f);
                    if (rowAnim > 0.004f) {
                        const float spineH = (cardHeight - 18.0f * uiScale) * rowAnim;
                        const float mid = (rowMin.y + rowMax.y) * 0.5f;
                        listDL->AddRectFilled(ImVec2(rowMin.x + 1.0f, mid - spineH * 0.5f),
                                              ImVec2(rowMin.x + 3.0f * uiScale, mid + spineH * 0.5f),
                                              LauncherSkin::U32(LauncherSkin::kAccentHi,
                                                                rowAlpha * rowAnim),
                                              1.5f * uiScale);
                    }

                    const float thumbPad = 8.0f * uiScale;
                    float textX = rowMin.x + cardPadX;
                    if (showThumbs) {
                        const ImVec2 thumbMin(rowMin.x + cardPadX, rowMin.y + thumbPad);
                        const ImVec2 thumbMax(thumbMin.x + thumbW, rowMax.y - thumbPad);
                        int previewTexWidth = 0;
                        int previewTexHeight = 0;
                        const ImTextureID previewTexId = resolveUiTexture(
                            getProjectPreviewPath(entry.recent.path), previewTexWidth, previewTexHeight);
                        if (previewTexId != static_cast<ImTextureID>(0)) {
                            DrawImageCover(listDL, previewTexId, thumbMin, thumbMax,
                                           previewTexWidth, previewTexHeight,
                                           LauncherSkin::U32(ImVec4(1, 1, 1, 1), rowAlpha),
                                           4.0f * uiScale);
                        } else {
                            LauncherShadeRect(listDL, thumbMin, thumbMax, LauncherSkin::kWell,
                                              ImGuiShadeClass_Frame, ImGuiShadeState_Normal,
                                              4.0f * uiScale, rowAlpha);
                            const char* nopv = "No Preview";
                            const float ns = baseFontSize * 0.80f;
                            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(ns, FLT_MAX, 0.0f, nopv);
                            listDL->AddText(ImGui::GetFont(), ns,
                                            ImVec2(thumbMin.x + (thumbW - ts.x) * 0.5f,
                                                   thumbMin.y + ((thumbMax.y - thumbMin.y) - ts.y) * 0.5f),
                                            LauncherSkin::U32(LauncherSkin::kTextFaint, rowAlpha),
                                            nopv);
                        }
                        listDL->AddRect(thumbMin, thumbMax,
                                        LauncherSkin::U32(LauncherSkin::kOutlineSoft, rowAlpha),
                                        4.0f * uiScale);
                        textX = thumbMax.x + 12.0f * uiScale;
                    }

                    const std::string displayName = entry.recent.name.empty()
                        ? (entry.metadata.name.empty() ? "(Unnamed Project)" : entry.metadata.name)
                        : entry.recent.name;
                    const fs::path rootPath = entry.projectRoot.empty()
                        ? fs::path(entry.recent.path)
                        : entry.projectRoot;
                    const std::string locationLabel = prefs.launcherShowFullPaths
                        ? rootPath.string()
                        : rootPath.filename().string();

                    const float openBtnW = 78.0f * uiScale;
                    const float openBtnH = 26.0f * uiScale;
                    const float openBtnX = rowMax.x - cardPadX - openBtnW;
                    const float textMaxX = openBtnX - 14.0f * uiScale;

                    listDL->AddText(ImGui::GetFont(), baseFontSize * 1.08f,
                                    ImVec2(textX, rowMin.y + 11.0f * uiScale),
                                    LauncherSkin::U32(LauncherSkin::kText, rowAlpha),
                                    displayName.c_str(), nullptr,
                                    std::max(0.0f, textMaxX - textX));

                    listDL->PushClipRect(ImVec2(textX, rowMin.y),
                                         ImVec2(std::max(textX, textMaxX), rowMax.y), true);
                    listDL->AddText(ImGui::GetFont(), baseFontSize * 0.86f,
                                    ImVec2(textX, rowMin.y + 30.0f * uiScale),
                                    LauncherSkin::U32(LauncherSkin::kTextFaint, rowAlpha),
                                    locationLabel.c_str());
                    listDL->PopClipRect();

                    float badgeX = textX;
                    const float badgeY = rowMax.y - 25.0f * uiScale;
                    badgeX += LauncherBadge(listDL, ImVec2(badgeX, badgeY),
                                            ProjectPipelineLabel(entry.metadata.pipeline),
                                            LauncherSkin::kAccentHi, uiScale, rowAlpha) + 5.0f * uiScale;
                    badgeX += LauncherBadge(listDL, ImVec2(badgeX, badgeY),
                                            Modularity::ToString(entry.metadata.rendererBackend),
                                            LauncherSkin::kTextDim, uiScale, rowAlpha) + 5.0f * uiScale;
                    if (!entry.metadata.found) {
                        LauncherBadge(listDL, ImVec2(badgeX, badgeY), "Unreadable",
                                      LauncherSkin::kWarn, uiScale, rowAlpha);
                    }

                    const std::string lastOpened = entry.recent.lastOpened.empty()
                        ? std::string("-") : entry.recent.lastOpened;
                    const float stampW = ImGui::GetFont()->CalcTextSizeA(
                        baseFontSize * 0.82f, FLT_MAX, 0.0f, lastOpened.c_str()).x;
                    listDL->AddText(ImGui::GetFont(), baseFontSize * 0.82f,
                                    ImVec2(rowMax.x - cardPadX - stampW, rowMin.y + 11.0f * uiScale),
                                    LauncherSkin::U32(LauncherSkin::kTextFaint, rowAlpha),
                                    lastOpened.c_str());

                    ImGui::SetCursorScreenPos(ImVec2(openBtnX, rowMax.y - cardPadX - openBtnH));
                    ImGui::SetNextItemAllowOverlap();
                    ImGui::BeginDisabled(launcherBusy);
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * rowAlpha);
                    if (LauncherButton("Open##Card",
                                       isSelected ? LauncherButtonKind::Primary
                                                  : LauncherButtonKind::Accent,
                                       ImVec2(openBtnW, openBtnH))) {
                        launcherSelectedProjectPath = entry.recent.path;
                        shouldOpen = true;
                    }
                    ImGui::PopStyleVar();
                    ImGui::EndDisabled();

                    if (shouldRemove) {
                        const auto it = std::find_if(
                            projectManager.recentProjects.begin(),
                            projectManager.recentProjects.end(),
                            [&](const RecentProject& rp) { return rp.path == entry.recent.path; });
                        if (it != projectManager.recentProjects.end()) {
                            projectManager.recentProjects.erase(it);
                            projectManager.saveRecentProjects();
                        }
                        launcherSelectedProjectPath.clear();
                        removedProject = true;
                        ImGui::PopID();
                        break;
                    }

                    if (shouldOpen && !launcherBusy) {
                        launchRecentProject(entry.recent, rowCenter);
                    }

                    ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, rowMax.y));
                    ImGui::Dummy(ImVec2(listWidth, cardGap));
                    ImGui::PopID();
                }

                if (visible.empty() && !removedProject) {
                    const char* headline = projectManager.recentProjects.empty()
                        ? "No projects tracked yet"
                        : "Nothing matches the current filter";
                    const char* body = projectManager.recentProjects.empty()
                        ? "Create a project or open an existing project.modu and it will show up here."
                        : "Clear the search box, or pick All Projects in the Library rail.";
                    ImGui::Dummy(ImVec2(0.0f, 22.0f * uiScale));
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kText);
                    ImGui::TextUnformatted(headline);
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                    ImGui::TextUnformatted(body);
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
            endGroupPanel();
            colX += listW + gap;

            if (showDetails) {
                ImGui::SetCursorPos(ImVec2(colX, colOrigin.y));
                beginGroupPanel("##ProjectDetailsPanel", ImVec2(detailW, availH), "Project Details");
                if (!selectedEntry) {
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                    ImGui::TextUnformatted("Select a project on the left to see where it lives, what "
                                           "it renders with and how much of it is on disk.");
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                } else {
                    const LauncherRecentProjectDisplayEntry& sel = *selectedEntry;
                    const fs::path selRoot = sel.projectRoot.empty()
                        ? fs::path(sel.recent.path) : sel.projectRoot;

                    {
                        const float previewH = 118.0f * uiScale;
                        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
                        const ImVec2 previewSize(ImGui::GetContentRegionAvail().x, previewH);
                        const ImVec2 previewMax(previewPos.x + previewSize.x,
                                                previewPos.y + previewSize.y);
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        int pw = 0;
                        int ph = 0;
                        const ImTextureID pid = resolveUiTexture(
                            getProjectPreviewPath(sel.recent.path), pw, ph);
                        if (pid != static_cast<ImTextureID>(0)) {
                            DrawImageCover(dl, pid, previewPos, previewMax, pw, ph,
                                           ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 5.0f * uiScale);
                        } else {
                            LauncherShadeRect(dl, previewPos, previewMax, LauncherSkin::kWell,
                                              ImGuiShadeClass_Frame, ImGuiShadeState_Normal,
                                              5.0f * uiScale);
                            const char* none = "No preview captured";
                            const ImVec2 ts = ImGui::CalcTextSize(none);
                            dl->AddText(ImVec2(previewPos.x + (previewSize.x - ts.x) * 0.5f,
                                               previewPos.y + (previewSize.y - ts.y) * 0.5f),
                                        LauncherSkin::U32(LauncherSkin::kTextFaint), none);
                        }
                        dl->AddRect(previewPos, previewMax,
                                    LauncherSkin::U32(LauncherSkin::kOutlineSoft), 5.0f * uiScale);
                        ImGui::Dummy(previewSize);
                    }

                    ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kText);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                    ImGui::TextUnformatted(sel.recent.name.empty() ? "(Unnamed Project)"
                                                                   : sel.recent.name.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));

                    const LauncherProjectStats& stats = GetLauncherProjectStats(selRoot);
                    LauncherDetailRow("Location", selRoot.string(), uiScale);
                    LauncherDetailRow("Pipeline", ProjectPipelineLabel(sel.metadata.pipeline), uiScale);
                    LauncherDetailRow("Renderer",
                                      Modularity::ToString(sel.metadata.rendererBackend), uiScale);
                    LauncherDetailRow("Last opened",
                                      sel.recent.lastOpened.empty() ? "-" : sel.recent.lastOpened,
                                      uiScale);
                    if (stats.valid) {
                        char contents[128];
                        std::snprintf(contents, sizeof(contents), "%d scene%s, %d script%s, %d package%s",
                                      stats.sceneCount, stats.sceneCount == 1 ? "" : "s",
                                      stats.scriptCount, stats.scriptCount == 1 ? "" : "s",
                                      stats.packageCount, stats.packageCount == 1 ? "" : "s");
                        LauncherDetailRow("Contents", contents, uiScale);
                        LauncherDetailRow("Size on disk",
                                          FormatBytesShort(stats.sizeBytes) +
                                              (stats.truncated ? " (partial)" : "") +
                                              "  -  excludes Cache and Builds",
                                          uiScale);
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kBad);
                        ImGui::TextUnformatted("Project folder is missing.");
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0.0f, 5.0f * uiScale));
                    }

                    const float footerH = 92.0f * uiScale;
                    const float remaining = ImGui::GetContentRegionAvail().y;
                    if (remaining > footerH) {
                        ImGui::Dummy(ImVec2(0.0f, remaining - footerH));
                    }
                    ImGui::Separator();
                    ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
                    ImGui::BeginDisabled(launcherBusy || !stats.valid);
                    if (LauncherButton("Open Project", LauncherButtonKind::Primary,
                                       ImVec2(-1.0f, 32.0f * uiScale))) {
                        const ImVec2 focus(windowPos.x + windowSize.x * 0.5f,
                                           windowPos.y + windowSize.y * 0.5f);
                        launchRecentProject(sel.recent, focus);
                    }
                    ImGui::EndDisabled();
                    const float halfW = (ImGui::GetContentRegionAvail().x -
                                         ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                    if (LauncherButton("Show Folder", LauncherButtonKind::Neutral,
                                       ImVec2(halfW, 28.0f * uiScale))) {
                        openInFileManager(selRoot);
                    }
                    ImGui::SameLine();
                    if (LauncherButton("Forget", LauncherButtonKind::Danger,
                                       ImVec2(halfW, 28.0f * uiScale))) {
                        const auto it = std::find_if(
                            projectManager.recentProjects.begin(),
                            projectManager.recentProjects.end(),
                            [&](const RecentProject& rp) { return rp.path == sel.recent.path; });
                        if (it != projectManager.recentProjects.end()) {
                            projectManager.recentProjects.erase(it);
                            projectManager.saveRecentProjects();
                        }
                        launcherSelectedProjectPath.clear();
                    }
                }
                endGroupPanel();
            }
        };

        auto renderNewProjectView = [&]() {
            static char templateSearch[128] = "";
            static int templateCategory = 0; // 0 = Blank Project, 1 = Template Projects
            const std::vector<LauncherTemplateEntry> templates = GatherTemplateEntries();
            const std::vector<LauncherTemplateEntry> builtInPresets = BuiltInProjectPresets(
                ProjectPipelineFromUiIndex(projectManager.preferences.defaultPipeline),
                projectManager.preferences.defaultRenderer == 1
                    ? Modularity::GraphicsBackend::Vulkan
                    : Modularity::GraphicsBackend::OpenGL);
            const fs::path templatesRoot = GetTemplateProjectsRoot();
            const std::string templateFilter = toLower(TrimCopy(templateSearch));

            LauncherTemplateEntry blankEntry = builtInPresets.empty() ? LauncherTemplateEntry{} : builtInPresets.front();

            auto selectTemplateEntry = [&](const LauncherTemplateEntry& entry) {
                if (entry.isBlankPreset) {
                    projectManager.newProjectTemplatePath.clear();
                    projectManager.newProjectTemplateName = entry.displayName;
                    projectManager.newProjectPresetId = entry.presetId.empty() ? "empty" : entry.presetId;
                } else {
                    projectManager.newProjectTemplatePath = entry.projectRoot.string();
                    projectManager.newProjectTemplateName = entry.displayName;
                    projectManager.newProjectPresetId.clear();
                }
                projectManager.newProjectPipelineMode = ProjectPipelineToUiIndex(entry.pipeline);
                projectManager.newProjectRendererMode = (entry.rendererBackend == Modularity::GraphicsBackend::Vulkan) ? 1 : 0;
#if !MODULARITY_HAS_VULKAN
                if (projectManager.newProjectRendererMode == 1) {
                    projectManager.newProjectRendererMode = 0;
                }
#else
                if (projectManager.newProjectRendererMode == 1 && !hasVulkanPipelinePackage()) {
                    projectManager.newProjectRendererMode = 0;
                }
#endif
                projectManager.newProjectImportLastPackages = false;
            };

            if (projectManager.newProjectTemplateName.empty()) {
                projectManager.newProjectTemplateName = blankEntry.displayName;
                projectManager.newProjectPresetId = blankEntry.presetId;
            }

            const LauncherTemplateEntry* selectedTemplate = nullptr;
            if (projectManager.newProjectTemplatePath.empty()) {
                selectedTemplate = &blankEntry;
                for (const auto& preset : builtInPresets) {
                    if (preset.presetId == projectManager.newProjectPresetId ||
                        preset.displayName == projectManager.newProjectTemplateName) {
                        selectedTemplate = &preset;
                        break;
                    }
                }
            } else {
                for (const auto& t : templates) {
                    if (fs::path(projectManager.newProjectTemplatePath) == t.projectRoot) {
                        selectedTemplate = &t;
                        break;
                    }
                }
            }
            if (!selectedTemplate) {
                projectManager.newProjectTemplatePath.clear();
                projectManager.newProjectTemplateName = blankEntry.displayName;
                projectManager.newProjectPresetId = blankEntry.presetId;
                selectedTemplate = &blankEntry;
            }
            projectManager.newProjectPipelineMode = ProjectPipelineToUiIndex(selectedTemplate->pipeline);
            projectManager.newProjectRendererMode = (selectedTemplate->rendererBackend == Modularity::GraphicsBackend::Vulkan) ? 1 : 0;
            projectManager.newProjectImportLastPackages = false;
#if !MODULARITY_HAS_VULKAN
            if (projectManager.newProjectRendererMode == 1) {
                projectManager.newProjectRendererMode = 0;
            }
#else
            if (projectManager.newProjectRendererMode == 1 && !hasVulkanPipelinePackage()) {
                projectManager.newProjectRendererMode = 0;
            }
#endif

            auto renderCategoryButton = [&](const char* label, int category, float width) {
                const bool selected = templateCategory == category;
                if (LauncherButton(label,
                                   selected ? LauncherButtonKind::Accent : LauncherButtonKind::Neutral,
                                   ImVec2(width, 28.0f * uiScale))) {
                    templateCategory = category;
                    if (templateCategory == 0) {
                        selectTemplateEntry(blankEntry);
                        selectedTemplate = &blankEntry;
                    } else if (selectedTemplate && selectedTemplate->isBlankPreset && !templates.empty()) {
                        selectTemplateEntry(templates.front());
                        selectedTemplate = &templates.front();
                    }
                }
            };

            auto resolvePreviewTexture = [&](const LauncherTemplateEntry& entry, ImTextureID& outId, int& outW, int& outH) {
                outId = static_cast<ImTextureID>(0);
                outW = 0;
                outH = 0;
                if (entry.previewImage.empty() || !fs::exists(entry.previewImage)) {
                    return;
                }
                if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
                    outId = vulkanRenderer->getOrCreateUIImage(entry.previewImage.string(), &outW, &outH);
                } else if (!usingVulkan()) {
                    if (Texture* templateTex = renderer.getTexture(entry.previewImage.string())) {
                        outId = (ImTextureID)(intptr_t)templateTex->GetID();
                        outW = templateTex->GetWidth();
                        outH = templateTex->GetHeight();
                    }
                }
            };

            auto renderTemplateListRow = [&](const LauncherTemplateEntry& entry) {
                const bool selected = selectedTemplate &&
                    ((entry.isBlankPreset && selectedTemplate->isBlankPreset &&
                      entry.presetId == selectedTemplate->presetId) ||
                     (!entry.isBlankPreset && !selectedTemplate->isBlankPreset &&
                      selectedTemplate->projectRoot == entry.projectRoot));

                ImGui::PushID(entry.isBlankPreset ? entry.presetId.c_str() : entry.projectRoot.string().c_str());
                const ImVec2 rowPos = ImGui::GetCursorScreenPos();
                const ImVec2 rowSize(std::max(48.0f * uiScale, ImGui::GetContentRegionAvail().x), 62.0f * uiScale);
                ImGui::InvisibleButton("TemplateSelectRow", rowSize);
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) {
                    selectTemplateEntry(entry);
                    selectedTemplate = &entry;
                }

                ImDrawList* list = ImGui::GetWindowDrawList();
                const ImVec2 rowMax(rowPos.x + rowSize.x, rowPos.y + rowSize.y);

                ImGuiStorage* tmplStore = ImGui::GetStateStorage();
                const ImGuiID tmplAnimKey = ImGui::GetID("##tmplAnim");
                float tmplAnim = tmplStore->GetFloat(tmplAnimKey, selected ? 1.0f : 0.0f);
                tmplAnim = SmoothApproach(tmplAnim, selected ? 1.0f : (hovered ? 0.6f : 0.0f),
                                          15.0f, frameDt);
                tmplStore->SetFloat(tmplAnimKey, tmplAnim);

                if (tmplAnim > 0.004f) {
                    LauncherShadeRect(list, rowPos, rowMax,
                                      selected ? LauncherSkin::kSelection : LauncherSkin::kCardHover,
                                      ImGuiShadeClass_Header,
                                      selected ? ImGuiShadeState_Selected : ImGuiShadeState_Hovered,
                                      5.0f * uiScale, tmplAnim);
                    const float spineH = (rowSize.y - 12.0f * uiScale) * tmplAnim;
                    const float mid = (rowPos.y + rowMax.y) * 0.5f;
                    list->AddRectFilled(ImVec2(rowPos.x, mid - spineH * 0.5f),
                                        ImVec2(rowPos.x + 3.0f * uiScale, mid + spineH * 0.5f),
                                        LauncherSkin::U32(LauncherSkin::kAccentHi, tmplAnim),
                                        1.5f * uiScale);
                }
                list->AddLine(ImVec2(rowPos.x, rowMax.y),
                              ImVec2(rowMax.x, rowMax.y),
                              LauncherSkin::U32(LauncherSkin::kSeam),
                              1.0f);

                const ImVec2 thumbMin(rowPos.x + 10.0f * uiScale, rowPos.y + 7.0f * uiScale);
                const ImVec2 thumbMax(thumbMin.x + 80.0f * uiScale, rowMax.y - 7.0f * uiScale);
                ImTextureID textureId = static_cast<ImTextureID>(0);
                int textureW = 0;
                int textureH = 0;
                resolvePreviewTexture(entry, textureId, textureW, textureH);
                if (textureId != static_cast<ImTextureID>(0)) {
                    DrawImageCover(list, textureId, thumbMin, thumbMax,
                                   textureW, textureH,
                                   ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 4.0f * uiScale);
                } else {
                    LauncherShadeRect(list, thumbMin, thumbMax, LauncherSkin::kWell,
                                      ImGuiShadeClass_Frame, ImGuiShadeState_Normal, 4.0f * uiScale);
                    const char* placeholder = entry.isBlankPreset ? "Default" : "Preview";
                    const ImVec2 textSize = ImGui::CalcTextSize(placeholder);
                    list->AddText(ImVec2(thumbMin.x + ((thumbMax.x - thumbMin.x) - textSize.x) * 0.5f,
                                         thumbMin.y + ((thumbMax.y - thumbMin.y) - textSize.y) * 0.5f),
                                  LauncherSkin::U32(LauncherSkin::kTextFaint),
                                  placeholder);
                }
                list->AddRect(thumbMin, thumbMax,
                              LauncherSkin::U32(LauncherSkin::kOutlineSoft), 4.0f * uiScale);

                list->AddText(ImVec2(thumbMax.x + 12.0f * uiScale, rowPos.y + 10.0f * uiScale),
                              LauncherSkin::U32(LauncherSkin::kText),
                              entry.displayName.c_str());
                list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.86f,
                              ImVec2(thumbMax.x + 12.0f * uiScale, rowPos.y + 31.0f * uiScale),
                              LauncherSkin::U32(LauncherSkin::kTextFaint),
                              ProjectPipelineLabel(entry.pipeline));
                ImGui::PopID();
            };

            static int templateViewMode = 1; // 0 = Grid, 1 = List
            auto renderTemplateViewButton = [&](const char* label, int mode, float width) {
                const bool selected = templateViewMode == mode;
                if (LauncherButton(label,
                                   selected ? LauncherButtonKind::Accent : LauncherButtonKind::Neutral,
                                   ImVec2(width, 24.0f * uiScale))) {
                    templateViewMode = mode;
                }
            };

            auto renderTemplateGridTile = [&](const LauncherTemplateEntry& entry, float tileWidth) {
                const bool selected = selectedTemplate &&
                    ((entry.isBlankPreset && selectedTemplate->isBlankPreset &&
                      entry.presetId == selectedTemplate->presetId) ||
                     (!entry.isBlankPreset && !selectedTemplate->isBlankPreset &&
                      selectedTemplate->projectRoot == entry.projectRoot));

                ImGui::PushID(entry.isBlankPreset ? entry.presetId.c_str() : entry.projectRoot.string().c_str());
                const float tileHeight = 114.0f * uiScale;
                const float previewHeight = 64.0f * uiScale;
                const ImVec2 tilePos = ImGui::GetCursorScreenPos();
                const ImVec2 tileSize(std::max(48.0f * uiScale, tileWidth), tileHeight);
                ImGui::InvisibleButton("TemplateGridTile", tileSize);
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) {
                    selectTemplateEntry(entry);
                    selectedTemplate = &entry;
                }

                ImDrawList* list = ImGui::GetWindowDrawList();
                const ImVec2 tileMax(tilePos.x + tileSize.x, tilePos.y + tileSize.y);
                const ImVec2 thumbMin(tilePos.x, tilePos.y);
                const ImVec2 thumbMax(tileMax.x, tilePos.y + previewHeight);

                ImGuiStorage* tileStore = ImGui::GetStateStorage();
                const ImGuiID tileAnimKey = ImGui::GetID("##tileAnim");
                float tileAnim = tileStore->GetFloat(tileAnimKey, selected ? 1.0f : 0.0f);
                tileAnim = SmoothApproach(tileAnim, selected ? 1.0f : (hovered ? 0.6f : 0.0f),
                                          15.0f, frameDt);
                tileStore->SetFloat(tileAnimKey, tileAnim);

                LauncherShadeRect(list, tilePos, tileMax, LauncherSkin::kCard,
                                  ImGuiShadeClass_Header, ImGuiShadeState_Normal, 5.0f * uiScale);
                if (tileAnim > 0.004f) {
                    LauncherShadeRect(list, tilePos, tileMax,
                                      selected ? LauncherSkin::kSelection : LauncherSkin::kCardHover,
                                      ImGuiShadeClass_Header,
                                      selected ? ImGuiShadeState_Selected : ImGuiShadeState_Hovered,
                                      5.0f * uiScale, tileAnim);
                }
                list->AddRect(tilePos, tileMax,
                              LauncherSkin::U32(selected ? LauncherSkin::kAccent
                                                         : LauncherSkin::kOutlineSoft),
                              5.0f * uiScale);
                if (tileAnim > 0.004f) {
                    const float capW = tileSize.x * tileAnim;
                    const float capMid = (tilePos.x + tileMax.x) * 0.5f;
                    list->AddRectFilled(ImVec2(capMid - capW * 0.5f, tilePos.y),
                                        ImVec2(capMid + capW * 0.5f, tilePos.y + 2.0f * uiScale),
                                        LauncherSkin::U32(LauncherSkin::kAccentHi, tileAnim));
                }

                ImTextureID textureId = static_cast<ImTextureID>(0);
                int textureW = 0;
                int textureH = 0;
                resolvePreviewTexture(entry, textureId, textureW, textureH);
                if (textureId != static_cast<ImTextureID>(0)) {
                    DrawImageCover(list, textureId, thumbMin, thumbMax,
                                   textureW, textureH,
                                   ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 5.0f * uiScale);
                } else {
                    LauncherShadeRect(list, thumbMin, thumbMax, LauncherSkin::kWell,
                                      ImGuiShadeClass_Frame, ImGuiShadeState_Normal, 5.0f * uiScale);
                    const char* placeholder = entry.isBlankPreset ? "Default" : "Preview";
                    const ImVec2 textSize = ImGui::CalcTextSize(placeholder);
                    list->AddText(ImVec2(thumbMin.x + ((thumbMax.x - thumbMin.x) - textSize.x) * 0.5f,
                                         thumbMin.y + ((thumbMax.y - thumbMin.y) - textSize.y) * 0.5f),
                                  LauncherSkin::U32(LauncherSkin::kTextFaint),
                                  placeholder);
                }

                const float titleWrapWidth = std::max(1.0f, tileSize.x - 16.0f * uiScale);
                list->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                              ImVec2(tilePos.x + 8.0f * uiScale, thumbMax.y + 8.0f * uiScale),
                              LauncherSkin::U32(LauncherSkin::kText),
                              entry.displayName.c_str(), nullptr, titleWrapWidth);
                list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.86f,
                              ImVec2(tilePos.x + 8.0f * uiScale, tileMax.y - 21.0f * uiScale),
                              LauncherSkin::U32(LauncherSkin::kTextFaint),
                              ProjectPipelineLabel(entry.pipeline));
                ImGui::PopID();
            };

            auto createProjectFromCurrentState = [&]() {
                if (std::strlen(projectManager.newProjectName) == 0) {
                    projectManager.errorMessage = "Hey! You'll need a Project name first!";
                } else if (std::strlen(projectManager.newProjectLocation) == 0) {
                    projectManager.errorMessage = "Hey! Please Set a default location in Settings first.";
                } else {
                    createNewProject(projectManager.newProjectName, projectManager.newProjectLocation);
                    if (projectManager.errorMessage.empty()) {
                        projectManager.showNewProjectDialog = false;
                    }
                }
            };

            const float headerRowY = ImGui::GetCursorPosY();
            ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kText);
            ImGui::TextUnformatted("Create New Project");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
            ImGui::TextUnformatted("Pick a starting point on the left, then confirm the name and "
                                   "location on the right.");
            ImGui::PopStyleColor();

            {
                const float catW = 128.0f * uiScale;
                const float tmplW = 158.0f * uiScale;
                const float cancelW = 96.0f * uiScale;
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                const float total = catW + tmplW + cancelW + spacing * 2.0f;
                if (ImGui::GetContentRegionAvail().x > total + 20.0f * uiScale) {
                    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() +
                                                   ImGui::GetContentRegionAvail().x - total,
                                               headerRowY + 4.0f * uiScale));
                } else {
                    ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));
                }
                renderCategoryButton("Defaults", 0, catW);
                ImGui::SameLine();
                renderCategoryButton("Template Projects", 1, tmplW);
                ImGui::SameLine();
                if (LauncherButton("Cancel", LauncherButtonKind::Neutral,
                                   ImVec2(cancelW, 28.0f * uiScale))) {
                    projectManager.showNewProjectDialog = false;
                    projectManager.errorMessage.clear();
                }
            }
            ImGui::Dummy(ImVec2(0.0f, 10.0f * uiScale));

            const ImVec2 splitOrigin = ImGui::GetCursorPos();
            const float splitAvailW = ImGui::GetContentRegionAvail().x;
            const float splitHeight = std::max(320.0f * uiScale, ImGui::GetContentRegionAvail().y);
            const float leftWidth = ImClamp(splitAvailW * 0.52f,
                                            340.0f * uiScale, 640.0f * uiScale);
            const float splitGap = 10.0f * uiScale;
            const float rightWidth = std::max(220.0f * uiScale, splitAvailW - leftWidth - splitGap);

            beginGroupPanel("NewProjectTemplateListPane", ImVec2(leftWidth, splitHeight),
                            templateCategory == 0 ? "Built-in Presets" : "Template Projects");
            {
                const float toggleWidth = 108.0f * uiScale;
                const float rowY = ImGui::GetCursorPosY();
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextDim);
                if (templateCategory == 1) {
                    ImGui::Text("Loaded from %s", templatesRoot.filename().string().c_str());
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                        ImGui::SetTooltip("%s", templatesRoot.string().c_str());
                    }
                } else {
                    ImGui::TextUnformatted("Clean starting points that ship with the engine.");
                }
                ImGui::PopStyleColor();
                if (ImGui::GetContentRegionAvail().x > toggleWidth + 12.0f * uiScale) {
                    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                    toggleWidth);
                    ImGui::SetCursorPosY(rowY - 2.0f * uiScale);
                    renderTemplateViewButton("Grid", 0, 50.0f * uiScale);
                    ImGui::SameLine(0.0f, 4.0f * uiScale);
                    renderTemplateViewButton("List", 1, 50.0f * uiScale);
                }
            }
            if (templateCategory == 1) {
                ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * uiScale);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, LauncherSkin::kWell);
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##TemplateSearch", "Search templates...",
                                         templateSearch, sizeof(templateSearch));
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));

            ImGui::PushStyleColor(ImGuiCol_ChildBg, LauncherSkin::kWell);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f * uiScale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * uiScale, 6.0f * uiScale));
            ImGui::BeginChild("TemplateRows", ImVec2(0.0f, 0.0f), true);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            if (templateCategory == 0) {
                if (templateViewMode == 0) {
                    int tileIndex = 0;
                    const float tileSpacing = 8.0f * uiScale;
                    const float contentRegionWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
                    const float minTileWidth = 146.0f * uiScale;
                    const int tileColumns = std::max(1, static_cast<int>((contentRegionWidth + tileSpacing) / (minTileWidth + tileSpacing)));
                    const float tileWidth = std::max(48.0f * uiScale,
                                                     (contentRegionWidth - tileSpacing * static_cast<float>(tileColumns - 1)) / static_cast<float>(tileColumns));
                    for (const auto& preset : builtInPresets) {
                        if (tileIndex > 0 && (tileIndex % tileColumns) != 0) ImGui::SameLine(0.0f, tileSpacing);
                        renderTemplateGridTile(preset, tileWidth);
                        ++tileIndex;
                    }
                } else {
                    for (const auto& preset : builtInPresets) renderTemplateListRow(preset);
                }
            } else {
                bool hadVisible = false;
                int tileIndex = 0;
                const float tileSpacing = 8.0f * uiScale;
                const float contentRegionWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
                const float minTileWidth = 146.0f * uiScale;
                const int tileColumns = std::max(1, static_cast<int>((contentRegionWidth + tileSpacing) / (minTileWidth + tileSpacing)));
                const float tileWidth = std::max(48.0f * uiScale,
                                                 (contentRegionWidth - tileSpacing * static_cast<float>(tileColumns - 1)) / static_cast<float>(tileColumns));
                for (const auto& t : templates) {
                    const std::string lowerName = toLower(t.displayName);
                    if (!templateFilter.empty() && lowerName.find(templateFilter) == std::string::npos) {
                        continue;
                    }
                    if (templateViewMode == 0) {
                        if (tileIndex > 0 && (tileIndex % tileColumns) != 0) {
                            ImGui::SameLine(0.0f, tileSpacing);
                        }
                        renderTemplateGridTile(t, tileWidth);
                        ++tileIndex;
                    } else {
                        renderTemplateListRow(t);
                    }
                    hadVisible = true;
                }
                if (!hadVisible) {
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                    if (templates.empty()) {
                        ImGui::TextUnformatted("No template projects found.");
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                        ImGui::TextUnformatted("Drop any folder containing a project.modu into "
                                               "Template-Projects and it will appear here.");
                        ImGui::PopTextWrapPos();
                    } else {
                        ImGui::TextUnformatted("No templates match your search.");
                    }
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
            endGroupPanel();

            ImGui::SetCursorPos(ImVec2(splitOrigin.x + leftWidth + splitGap, splitOrigin.y));
            beginGroupPanel("NewProjectTemplateDetailPane", ImVec2(rightWidth, splitHeight),
                            "Starting Point");
            {
                const float previewHeight = std::min(150.0f * uiScale,
                                                     ImGui::GetContentRegionAvail().y * 0.26f);
                const ImVec2 previewPos = ImGui::GetCursorScreenPos();
                const ImVec2 previewSize(ImGui::GetContentRegionAvail().x, previewHeight);
                const ImVec2 previewMax(previewPos.x + previewSize.x, previewPos.y + previewSize.y);
                ImDrawList* list = ImGui::GetWindowDrawList();

                ImTextureID previewTexId = static_cast<ImTextureID>(0);
                int previewTexWidth = 0;
                int previewTexHeight = 0;
                resolvePreviewTexture(*selectedTemplate, previewTexId, previewTexWidth, previewTexHeight);
                if (previewTexId != static_cast<ImTextureID>(0)) {
                    DrawImageCover(list, previewTexId, previewPos, previewMax,
                                   previewTexWidth, previewTexHeight,
                                   ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 5.0f * uiScale);
                } else {
                    LauncherShadeRect(list, previewPos, previewMax, LauncherSkin::kWell,
                                      ImGuiShadeClass_Frame, ImGuiShadeState_Normal, 5.0f * uiScale);
                    const std::string placeholder = selectedTemplate->isBlankPreset
                        ? std::string("Standard ") +
                              ProjectPipelineLabel(selectedTemplate->pipeline) + " project"
                        : std::string("No preview available");
                    const ImVec2 textSize = ImGui::CalcTextSize(placeholder.c_str());
                    list->AddText(ImVec2(previewPos.x + (previewSize.x - textSize.x) * 0.5f,
                                         previewPos.y + (previewSize.y - textSize.y) * 0.5f),
                                  LauncherSkin::U32(LauncherSkin::kTextFaint),
                                  placeholder.c_str());
                }
                list->AddRect(previewPos, previewMax,
                              LauncherSkin::U32(LauncherSkin::kOutlineSoft), 5.0f * uiScale);
                ImGui::Dummy(previewSize);
            }

            std::string selectedDescription = selectedTemplate->description;
            if (selectedDescription.empty()) {
                selectedDescription = selectedTemplate->isBlankPreset
                    ? "A clean Modularity project: the standard folder layout, an empty scene, and nothing else to unpick."
                    : "Copies the selected template project and renames it. Everything it ships with - scenes, scripts, assets and package manifest - comes along.";
            }
            if (selectedTemplate->isBlankPreset && selectedTemplate->presetId == "empty") {
                selectedDescription += "  Its pipeline and renderer follow the New Project defaults in Settings.";
            }
            const std::string sourceLabel = selectedTemplate->isBlankPreset
                ? "Built-in preset"
                : selectedTemplate->projectRoot.filename().string();
            ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));
            ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kText);
            ImGui::TextUnformatted(selectedTemplate->displayName.c_str());
            ImGui::PopStyleColor();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextDim);
            ImGui::TextUnformatted(selectedDescription.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));

            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 badgePos = ImGui::GetCursorScreenPos();
                const float startX = badgePos.x;
                const float badgeH = ImGui::GetFontSize() * 0.82f + 6.0f * uiScale;
                badgePos.x += LauncherBadge(dl, badgePos,
                                            ProjectPipelineLabel(ProjectPipelineFromUiIndex(
                                                projectManager.newProjectPipelineMode)),
                                            LauncherSkin::kAccentHi, uiScale) + 5.0f * uiScale;
                badgePos.x += LauncherBadge(dl, badgePos,
                                            Modularity::ToString(
                                                projectManager.newProjectRendererMode == 1
                                                    ? Modularity::GraphicsBackend::Vulkan
                                                    : Modularity::GraphicsBackend::OpenGL),
                                            LauncherSkin::kTextDim, uiScale) + 5.0f * uiScale;
                LauncherBadge(dl, badgePos, sourceLabel.c_str(), LauncherSkin::kTextFaint, uiScale);
                ImGui::Dummy(ImVec2(badgePos.x - startX, badgeH));
            }

            ImGui::Dummy(ImVec2(0.0f, 10.0f * uiScale));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * uiScale);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, LauncherSkin::kWell);
            LauncherSettingLabel("Project name",
                                 "Also the folder name created inside the location below.", uiScale);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##ProjectNameInline", "My Project",
                                     projectManager.newProjectName,
                                     sizeof(projectManager.newProjectName));
            ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));

            LauncherSettingLabel("Location",
                                 "Change the default for every new project in Settings > Paths.",
                                 uiScale);
            const float browseWidth = 92.0f * uiScale;
            ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
            ImGui::InputText("##ProjectLocationInline", projectManager.newProjectLocation,
                             sizeof(projectManager.newProjectLocation));
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::SameLine();
            if (LauncherButton("Browse##ProjectLocationInline", LauncherButtonKind::Neutral,
                               ImVec2(browseWidth, 0.0f))) {
                openInFileManager(fs::path(projectManager.newProjectLocation));
            }

            if (!projectManager.errorMessage.empty()) {
                ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kBad);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                ImGui::TextUnformatted(projectManager.errorMessage.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }

            if (std::strlen(projectManager.newProjectName) > 0 &&
                std::strlen(projectManager.newProjectLocation) > 0) {
                ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
                const fs::path previewRoot = fs::path(projectManager.newProjectLocation) /
                                             projectManager.newProjectName;
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                ImGui::Text("Creates %s", previewRoot.string().c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }

            {
                const size_t defaultPkgs = projectManager.preferences.applyDefaultPackages
                    ? projectManager.preferences.defaultPackageIds.size() : 0;
                if (defaultPkgs > 0) {
                    ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kAccentHi);
                    ImGui::Text("%zu default ModuPAK%s will be installed.",
                                defaultPkgs, defaultPkgs == 1 ? "" : "s");
                    ImGui::PopStyleColor();
                }
            }

            {
                const float footerH = 42.0f * uiScale;
                const float remaining = ImGui::GetContentRegionAvail().y;
                if (remaining > footerH) ImGui::Dummy(ImVec2(0.0f, remaining - footerH));
                const float cancelWidth = 100.0f * uiScale;
                const float createWidth = 152.0f * uiScale;
                const float actionsWidth = cancelWidth + createWidth + ImGui::GetStyle().ItemSpacing.x;
                const float actionsOffset = std::max(0.0f, ImGui::GetContentRegionAvail().x - actionsWidth);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + actionsOffset);
                if (LauncherButton("Cancel##NewProjectFooter", LauncherButtonKind::Neutral,
                                   ImVec2(cancelWidth, 32.0f * uiScale))) {
                    projectManager.showNewProjectDialog = false;
                    projectManager.errorMessage.clear();
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(launcherBusy);
                if (LauncherButton("Create Project", LauncherButtonKind::Primary,
                                   ImVec2(createWidth, 32.0f * uiScale))) {
                    createProjectFromCurrentState();
                }
                ImGui::EndDisabled();
            }
            endGroupPanel();
        };

        auto renderPackagesView = [&]() {
            static int packageCategory = 0;
            static char packageSearch[128] = "";
            static std::string selectedPackageId;
            static std::string packageStatus;
            static bool packageStatusError = false;

            EditorGlobalPreferences& prefs = projectManager.preferences;
            const std::vector<PackageInfo>& registry = packageManager.getRegistry();

            struct PackageRow {
                const PackageInfo* pkg = nullptr;
                bool global = false;
                bool isDefault = false;
                bool compatible = true;
                bool inProject = false;
            };

            const std::string search = toLower(TrimCopy(packageSearch));
            std::vector<PackageRow> rows;
            int counts[4] = {0, 0, 0, 0};
            rows.reserve(registry.size());
            for (const PackageInfo& pkg : registry) {
                if (!pkg.registryPackage) continue;
                PackageRow row;
                row.pkg = &pkg;
                row.global = packageManager.isGloballyInstalled(pkg.id);
                row.isDefault = prefs.isDefaultPackage(pkg.id);
                row.compatible = packageManager.isCompatible(pkg);
                row.inProject = packageManager.isInstalled(pkg.id);

                ++counts[0];
                if (row.global) ++counts[1];
                if (row.isDefault) ++counts[2];
                if (!row.compatible) ++counts[3];

                bool inCategory = true;
                if (packageCategory == 1) inCategory = row.global;
                else if (packageCategory == 2) inCategory = row.isDefault;
                else if (packageCategory == 3) inCategory = !row.compatible;
                if (!inCategory) continue;

                if (!search.empty()) {
                    const std::string haystack = toLower(pkg.name + " " + pkg.id + " " +
                                                         pkg.author + " " + pkg.subsystem + " " +
                                                         pkg.packageType);
                    if (haystack.find(search) == std::string::npos) continue;
                }
                rows.push_back(row);
            }
            std::stable_sort(rows.begin(), rows.end(), [&](const PackageRow& a, const PackageRow& b) {
                return toLower(a.pkg->name.empty() ? a.pkg->id : a.pkg->name) <
                       toLower(b.pkg->name.empty() ? b.pkg->id : b.pkg->name);
            });

            const PackageRow* selectedRow = nullptr;
            for (const PackageRow& row : rows) {
                if (row.pkg->id == selectedPackageId) { selectedRow = &row; break; }
            }
            if (!selectedRow && !rows.empty()) {
                selectedRow = &rows.front();
                selectedPackageId = selectedRow->pkg->id;
            } else if (rows.empty()) {
                selectedPackageId.clear();
            }

            const ImVec2 colOrigin = ImGui::GetCursorPos();
            const float availW = ImGui::GetContentRegionAvail().x;
            const float availH = ImGui::GetContentRegionAvail().y;
            const float railW = 244.0f * uiScale;
            const float detailW = 336.0f * uiScale;
            const float gap = 10.0f * uiScale;
            const float listMinW = 340.0f * uiScale;
            const bool showDetails = availW > (detailW + gap + listMinW);
            const bool showRail = availW > (railW + gap + detailW + gap + listMinW);
            const float listW = availW
                              - (showRail ? railW + gap : 0.0f)
                              - (showDetails ? detailW + gap : 0.0f);
            float colX = colOrigin.x;

            if (showRail) {
                const float optionsH = 210.0f * uiScale;
                const float catH = std::max(160.0f * uiScale, availH - optionsH - gap);

                ImGui::SetCursorPos(ImVec2(colX, colOrigin.y));
                beginGroupPanel("##PkgRailCategories", ImVec2(railW, catH), "Views");
                struct CatDef { const char* label; const char* hint; int mode; };
                const CatDef cats[] = {
                    {"All Packages",     "Everything available",   0},
                    {"Installed Global", "Shared by all projects", 1},
                    {"Default Set",      "Added to new projects",  2},
                    {"Incompatible",     "Wrong engine version",   3},
                };
                for (const CatDef& c : cats) {
                    if (LauncherRailItem(c.label, c.label, c.hint,
                                         packageCategory == c.mode, counts[c.mode], uiScale)) {
                        packageCategory = c.mode;
                    }
                }
                endGroupPanel();

                ImGui::SetCursorPos(ImVec2(colX, colOrigin.y + catH + gap));
                beginGroupPanel("##PkgRailOptions", ImVec2(railW, optionsH), "New Projects");
                bool applyDefaults = prefs.applyDefaultPackages;
                if (ImGui::Checkbox("Apply on create", &applyDefaults)) {
                    prefs.applyDefaultPackages = applyDefaults;
                    projectManager.saveLauncherSettings();
                }
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                ImGui::TextUnformatted("New projects install every package marked Default.");
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextDim);
                ImGui::Text("%d in the default set", counts[2]);
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
                if (LauncherButton("Refresh Registry", LauncherButtonKind::Neutral,
                                   ImVec2(-1.0f, 26.0f * uiScale))) {
                    packageManager.refreshRegistry();
                    (void)GetLauncherPackageSnapshot(projectManager, true);
                    packageStatus = "Registry reloaded.";
                    packageStatusError = false;
                }
                endGroupPanel();
                colX += railW + gap;
            }

            ImGui::SetCursorPos(ImVec2(colX, colOrigin.y));
            beginGroupPanel("##PkgListPanel", ImVec2(listW, availH), "Installed ModuPAKs");
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * uiScale);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, LauncherSkin::kWell);
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, LauncherSkin::kPanel);
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, LauncherSkin::kPanel);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##PackageSearch", "Search packages, authors or subsystems...",
                                         packageSearch, sizeof(packageSearch));
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar();

                ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                ImGui::Text("%zu package%s  -  global store for ModuEngine %s",
                            rows.size(), rows.size() == 1 ? "" : "s", engineVersion.c_str());
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
            }

            ImGui::PushStyleColor(ImGuiCol_ChildBg, LauncherSkin::kWell);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f * uiScale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * uiScale, 6.0f * uiScale));
            ImGui::BeginChild("PackageRows", ImVec2(0.0f, 0.0f), true);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();

            if (rows.empty()) {
                ImGui::Dummy(ImVec2(0.0f, 16.0f * uiScale));
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                ImGui::TextUnformatted(packageManager.hasRegistryMetadata()
                    ? "No packages match this view. Try All Packages, or clear the search box."
                    : "No ModuEngine registry was found next to this build, so there is nothing to "
                      "list yet. Refresh once the registry is available.");
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            } else {
                ImDrawList* rowDL = ImGui::GetWindowDrawList();
                const float rowW = ImGui::GetContentRegionAvail().x;
                const float rowH = 52.0f * uiScale;
                for (const PackageRow& row : rows) {
                    const PackageInfo& pkg = *row.pkg;
                    const bool isSelected = pkg.id == selectedPackageId;
                    ImGui::PushID(pkg.id.c_str());
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##pkgRow", ImVec2(rowW, rowH));
                    const bool hovered = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked()) selectedPackageId = pkg.id;
                    const ImVec2 max(pos.x + rowW, pos.y + rowH);

                    ImGuiStorage* store = ImGui::GetStateStorage();
                    const ImGuiID animKey = ImGui::GetID("##pkgAnim");
                    float anim = store->GetFloat(animKey, isSelected ? 1.0f : 0.0f);
                    anim = SmoothApproach(anim, isSelected ? 1.0f : (hovered ? 0.6f : 0.0f),
                                          15.0f, frameDt);
                    store->SetFloat(animKey, anim);

                    LauncherShadeRect(rowDL, pos, max, LauncherSkin::kCard,
                                      ImGuiShadeClass_Header, ImGuiShadeState_Normal, 5.0f * uiScale);
                    if (anim > 0.004f) {
                        LauncherShadeRect(rowDL, pos, max,
                                          isSelected ? LauncherSkin::kSelection
                                                     : LauncherSkin::kCardHover,
                                          ImGuiShadeClass_Header,
                                          isSelected ? ImGuiShadeState_Selected
                                                     : ImGuiShadeState_Hovered,
                                          5.0f * uiScale, anim);
                        rowDL->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y + 6.0f * uiScale),
                                             ImVec2(pos.x + 3.0f * uiScale, max.y - 6.0f * uiScale),
                                             LauncherSkin::U32(LauncherSkin::kAccentHi, anim),
                                             1.5f * uiScale);
                    }

                    const std::string title = pkg.name.empty() ? pkg.id : pkg.name;
                    const float pad = 11.0f * uiScale;
                    rowDL->AddText(ImVec2(pos.x + pad, pos.y + 8.0f * uiScale),
                                   LauncherSkin::U32(row.compatible ? LauncherSkin::kText
                                                                    : LauncherSkin::kTextDim),
                                   title.c_str());
                    std::string sub = pkg.id;
                    if (!pkg.author.empty()) sub = pkg.author + "  -  " + sub;
                    rowDL->AddText(ImGui::GetFont(), baseFontSize * 0.84f,
                                   ImVec2(pos.x + pad, pos.y + 28.0f * uiScale),
                                   LauncherSkin::U32(LauncherSkin::kTextFaint), sub.c_str());

                    float badgeRight = max.x - pad;
                    auto badgeRTL = [&](const char* text, const ImVec4& tint) {
                        const float w = ImGui::GetFont()->CalcTextSizeA(
                                            baseFontSize * 0.82f, FLT_MAX, 0.0f, text).x +
                                        16.0f * uiScale;
                        badgeRight -= w;
                        LauncherBadge(rowDL, ImVec2(badgeRight, pos.y + 15.0f * uiScale),
                                      text, tint, uiScale);
                        badgeRight -= 5.0f * uiScale;
                    };
                    if (!row.compatible)   badgeRTL("Incompatible", LauncherSkin::kBad);
                    if (row.isDefault)     badgeRTL("Default", LauncherSkin::kGold);
                    if (row.global)        badgeRTL("Global", LauncherSkin::kGood);
                    if (row.inProject)     badgeRTL("In Project", LauncherSkin::kAccentHi);
                    if (!pkg.version.empty()) {
                        const std::string v = "v" + pkg.version;
                        const float vw = ImGui::GetFont()->CalcTextSizeA(
                            baseFontSize * 0.82f, FLT_MAX, 0.0f, v.c_str()).x;
                        badgeRight -= vw;
                        rowDL->AddText(ImGui::GetFont(), baseFontSize * 0.82f,
                                       ImVec2(badgeRight, pos.y + 18.0f * uiScale),
                                       LauncherSkin::U32(LauncherSkin::kTextDim), v.c_str());
                    }

                    ImGui::PopID();
                    ImGui::Dummy(ImVec2(rowW, 5.0f * uiScale));
                }
            }
            ImGui::EndChild();
            endGroupPanel();
            colX += listW + gap;

            if (showDetails) {
                ImGui::SetCursorPos(ImVec2(colX, colOrigin.y));
                beginGroupPanel("##PkgDetailPanel", ImVec2(detailW, availH), "Package Details");
                if (!selectedRow) {
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                    ImGui::TextUnformatted("Pick a package to see what it provides, where its files "
                                           "come from, and whether it is compatible with this build.");
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                } else {
                    const PackageInfo& pkg = *selectedRow->pkg;
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kText);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                    ImGui::TextUnformatted(pkg.name.empty() ? pkg.id.c_str() : pkg.name.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));

                    if (!pkg.description.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextDim);
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                        ImGui::TextUnformatted(pkg.description.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));
                    }

                    LauncherDetailRow("Identifier", pkg.id, uiScale);
                    LauncherDetailRow("Author", pkg.author, uiScale);
                    LauncherDetailRow("Version", pkg.version.empty() ? "-" : ("v" + pkg.version), uiScale);
                    if (!pkg.subsystem.empty()) LauncherDetailRow("Subsystem", pkg.subsystem, uiScale);
                    if (!pkg.packageType.empty()) LauncherDetailRow("Type", pkg.packageType, uiScale);
                    LauncherDetailRow("Requires engine",
                                      pkg.compatibleModuEngineVersion.empty()
                                          ? std::string("Any")
                                          : pkg.compatibleModuEngineVersion,
                                      uiScale);

                    std::string state;
                    if (selectedRow->global) state = "Installed globally";
                    if (selectedRow->inProject) {
                        state += state.empty() ? "Installed in the open project"
                                               : " and in the open project";
                    }
                    if (state.empty()) state = "Not installed";
                    LauncherDetailRow("Status", state, uiScale);

                    std::string source;
                    if (!pkg.registrySourcePath.empty()) source = "Local registry checkout";
                    if (!pkg.downloadUrl.empty()) {
                        source += source.empty() ? "Online archive" : " + online archive";
                    }
                    if (source.empty() && !pkg.gitUrl.empty()) source = pkg.gitUrl;
                    LauncherDetailRow("Source", source.empty() ? "Unavailable" : source, uiScale);

                    if (!selectedRow->compatible) {
                        ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kBad);
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                        ImGui::Text("Built for a different ModuEngine version. Installing it here "
                                    "would give scripts headers this build cannot satisfy.");
                        ImGui::PopTextWrapPos();
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
                    }

                    const float footerH = 128.0f * uiScale;
                    const float remaining = ImGui::GetContentRegionAvail().y;
                    if (remaining > footerH) ImGui::Dummy(ImVec2(0.0f, remaining - footerH));
                    ImGui::Separator();
                    ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));

                    bool isDefault = selectedRow->isDefault;
                    if (ImGui::Checkbox("Default for new projects", &isDefault)) {
                        prefs.setDefaultPackage(pkg.id, isDefault);
                        projectManager.saveLauncherSettings();
                        packageStatus = isDefault
                            ? (pkg.id + " will be installed into new projects.")
                            : (pkg.id + " removed from the default set.");
                        packageStatusError = false;
                    }
                    ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));

                    ImGui::BeginDisabled(!selectedRow->compatible || selectedRow->global);
                    if (LauncherButton("Install Globally", LauncherButtonKind::Primary,
                                       ImVec2(-1.0f, 30.0f * uiScale))) {
                        if (packageManager.installRegistryPackageGlobally(pkg.id)) {
                            packageStatus = "Installed " + pkg.id + " into the global store.";
                            packageStatusError = false;
                            playEditorFeedbackOneShot("Resources/Sounds/Modupak Success installed.mp3",
                                                      0.85f, EditorFeedbackSoundCategory::Other);
                        } else {
                            packageStatus = packageManager.getLastError().empty()
                                ? std::string("Global install failed.")
                                : packageManager.getLastError();
                            packageStatusError = true;
                        }
                    }
                    ImGui::EndDisabled();

                    ImGui::BeginDisabled(!selectedRow->global);
                    if (LauncherButton("Remove From Global Store", LauncherButtonKind::Danger,
                                       ImVec2(-1.0f, 26.0f * uiScale))) {
                        if (packageManager.removeRegistryPackageGlobally(pkg.id)) {
                            packageStatus = "Removed " + pkg.id + " from the global store.";
                            packageStatusError = false;
                        } else {
                            packageStatus = packageManager.getLastError().empty()
                                ? std::string("Global removal failed.")
                                : packageManager.getLastError();
                            packageStatusError = true;
                        }
                    }
                    ImGui::EndDisabled();

                    if (!packageStatus.empty()) {
                        ImGui::Dummy(ImVec2(0.0f, 5.0f * uiScale));
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              packageStatusError ? LauncherSkin::kBad
                                                                 : LauncherSkin::kGood);
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                        ImGui::TextUnformatted(packageStatus.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::PopStyleColor();
                    }
                }
                endGroupPanel();
            }
        };

        auto renderSettingsView = [&]() {
            static int settingsCategory = 0;
            static std::string settingsStatus;
            static bool settingsStatusError = false;

            EditorGlobalPreferences& prefs = projectManager.preferences;
            bool prefsDirty = false;

            const ImVec2 colOrigin = ImGui::GetCursorPos();
            const float availW = ImGui::GetContentRegionAvail().x;
            const float availH = ImGui::GetContentRegionAvail().y;
            const float railW = 248.0f * uiScale;
            const float gap = 10.0f * uiScale;
            const bool showRail = availW > (railW + gap + 380.0f * uiScale);
            const float bodyW = availW - (showRail ? railW + gap : 0.0f);
            float colX = colOrigin.x;

            struct CatDef { const char* label; const char* hint; };
            const CatDef cats[] = {
                {"Appearance",  "Theme, font, scaling"},
                {"Launcher",    "This screen's behaviour"},
                {"Editor",      "Viewport, console, tools"},
                {"Performance", "Pacing and scripts"},
                {"Projects",    "Defaults for new work"},
                {"Sound",       "Editor feedback audio"},
                {"Language",    "Editor and ModuCPP"},
                {"Paths",       "Where files live"},
            };
            const int catCount = static_cast<int>(sizeof(cats) / sizeof(cats[0]));

            if (showRail) {
                ImGui::SetCursorPos(ImVec2(colX, colOrigin.y));
                beginGroupPanel("##SettingsRail", ImVec2(railW, availH), "Settings");
                for (int i = 0; i < catCount; ++i) {
                    if (LauncherRailItem(cats[i].label, cats[i].label, cats[i].hint,
                                         settingsCategory == i, 0, uiScale)) {
                        settingsCategory = i;
                    }
                }
                ImGui::Dummy(ImVec2(0.0f, 10.0f * uiScale));
                if (LauncherButton("Reset All to Defaults", LauncherButtonKind::Danger,
                                   ImVec2(-1.0f, 26.0f * uiScale))) {
                    prefs = EditorGlobalPreferences{};
                    prefsDirty = true;
                    settingsStatus = "Preferences reset to their defaults.";
                    settingsStatusError = false;
                }
                endGroupPanel();
                colX += railW + gap;
            }

            ImGui::SetCursorPos(ImVec2(colX, colOrigin.y));
            beginGroupPanel("##SettingsBody", ImVec2(bodyW, availH),
                            showRail ? cats[settingsCategory].label : "Settings");
            if (!showRail) {
                ImGui::SetNextItemWidth(-1.0f);
                const char* comboLabels[] = {"Appearance", "Launcher", "Editor", "Performance",
                                             "Projects", "Sound", "Language", "Paths"};
                ImGui::Combo("##SettingsCategory", &settingsCategory, comboLabels, catCount);
                ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));
            }

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::BeginChild("##SettingsScroll", ImVec2(0.0f, -40.0f * uiScale), false);
            ImGui::PopStyleColor();

            const float controlW = std::min(340.0f * uiScale,
                                            std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.6f));
            auto sectionHeading = [&](const char* title, const char* blurb) {
                ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kAccentHi);
                ImGui::TextUnformatted(title);
                ImGui::PopStyleColor();
                if (blurb && *blurb) {
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                    ImGui::TextUnformatted(blurb);
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));
            };
            auto checkboxSetting = [&](const char* label, const char* hint, bool* value) {
                LauncherSettingLabel(label, hint, uiScale);
                ImGui::PushID(label);
                if (ImGui::Checkbox("##value", value)) prefsDirty = true;
                ImGui::PopID();
                ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));
            };

            switch (settingsCategory) {
                case 0: {
                    sectionHeading("Theme",
                                   "Applies to the editor immediately. A project that stores its own "
                                   "theme keeps using that one.");
                    LauncherSettingLabel("Editor theme",
                                         "Default is the shaded Modularity look shown here.", uiScale);
                    {
                        std::vector<const char*> names;
                        int current = 0;
                        names.reserve(uiStylePresets.size());
                        for (size_t i = 0; i < uiStylePresets.size(); ++i) {
                            names.push_back(uiStylePresets[i].name.c_str());
                            if (uiStylePresets[i].name == prefs.themePreset) current = static_cast<int>(i);
                        }
                        if (!names.empty()) {
                            ImGui::SetNextItemWidth(controlW);
                            if (ImGui::Combo("##ThemePreset", &current, names.data(),
                                             static_cast<int>(names.size()))) {
                                prefs.themePreset = names[current];
                                prefsDirty = true;
                            }
                        } else {
                            ImGui::TextDisabled("No presets are loaded yet.");
                        }
                    }
                    ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));

                    LauncherSettingLabel("Interface font",
                                         "Any font in Resources or a project's font folder.", uiScale);
                    {
                        std::vector<const char*> fontLabels;
                        std::vector<const std::string*> fontIds;
                        int current = 0;
                        for (size_t i = 0; i < uiFontCatalog.size(); ++i) {
                            fontLabels.push_back(uiFontCatalog[i].label.c_str());
                            fontIds.push_back(&uiFontCatalog[i].id);
                            if (uiFontCatalog[i].id == prefs.uiFontAsset) current = static_cast<int>(i);
                        }
                        if (!fontLabels.empty()) {
                            ImGui::SetNextItemWidth(controlW);
                            if (ImGui::Combo("##UiFont", &current, fontLabels.data(),
                                             static_cast<int>(fontLabels.size()))) {
                                prefs.uiFontAsset = *fontIds[current];
                                prefsDirty = true;
                            }
                        } else {
                            ImGui::TextDisabled("Font catalog is empty.");
                        }
                    }
                    ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));

                    LauncherSettingLabel("Chrome size",
                                         "How tall menus, toolbars and tabs are. Big is the touch-friendly one.",
                                         uiScale);
                    {
                        const char* labels[] = {"Compact", "Default", "Big"};
                        int current = prefs.chromeScale == "Compact" ? 0
                                    : (prefs.chromeScale == "Big" ? 2 : 1);
                        ImGui::SetNextItemWidth(controlW);
                        if (ImGui::Combo("##ChromeScale", &current, labels, 3)) {
                            prefs.chromeScale = labels[current];
                            prefsDirty = true;
                        }
                    }
                    ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));

                    LauncherSettingLabel("Animation",
                                         "Fluid eases everything, Snappy shortens it, Off removes it entirely.",
                                         uiScale);
                    {
                        const char* labels[] = {"Off", "Snappy", "Fluid"};
                        int current = prefs.animationMode == "Off" ? 0
                                    : (prefs.animationMode == "Snappy" ? 1 : 2);
                        ImGui::SetNextItemWidth(controlW);
                        if (ImGui::Combo("##AnimMode", &current, labels, 3)) {
                            prefs.animationMode = labels[current];
                            prefsDirty = true;
                        }
                    }
                    break;
                }
                case 1: {
                    sectionHeading("This screen",
                                   "How the project manager behaves the next time it opens.");
                    checkboxSetting("Play the startup animation",
                                    "The logo build-in and chime when the launcher first appears.",
                                    &prefs.launcherIntroAnimation);
                    checkboxSetting("Show project thumbnails",
                                    "Turn off for a text-only list that loads no preview textures.",
                                    &prefs.launcherProjectThumbnails);
                    checkboxSetting("Show full project paths",
                                    "Off shows only the folder name under each project.",
                                    &prefs.launcherShowFullPaths);
                    checkboxSetting("Remember the last tab",
                                    "Reopen on whichever of Projects, ModuPAK or Settings you left on.",
                                    &prefs.launcherRememberLastTab);
                    checkboxSetting("Reopen the last project on start",
                                    "Skips this screen and loads the most recent project directly.",
                                    &prefs.reopenLastProject);
                    break;
                }
                case 2: {
                    sectionHeading("Workspace",
                                   "Defaults for every project. A project that has already saved its "
                                   "own editor settings keeps them.");
                    checkboxSetting("Scene gizmos",
                                    "Draw move/rotate/scale handles and object markers in the viewport.",
                                    &prefs.sceneGizmos);
                    checkboxSetting("Camera overlays on gizmos",
                                    "Frustum outlines and preview thumbnails for camera objects.",
                                    &prefs.gizmoCameraOverlays);
                    checkboxSetting("Texture previews in the hierarchy",
                                    "Small thumbnails beside sprite and texture entries.",
                                    &prefs.hierarchyTexturePreviews);
                    checkboxSetting("Wrap console text",
                                    "Long log lines wrap instead of scrolling sideways.",
                                    &prefs.consoleWrapText);
                    checkboxSetting("Pin the quick tools bar",
                                    "Keeps the floating tool strip visible instead of auto-hiding it.",
                                    &prefs.quickToolsPinned);
                    break;
                }
                case 3: {
                    sectionHeading("Hardware",
                                   "What this machine measured, and how hard the editor leans on it. "
                                   "Low turns off frosted glass and starts new projects at reduced "
                                   "quality.");
                    {
                        namespace HP = Modularity::HardwareProfile;
                        const HP::StaticInfo& hw = HP::Static();

                        ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                               ImGui::GetContentRegionAvail().x);
                        ImGui::TextUnformatted(hw.glRenderer.empty()
                                                   ? "Graphics device not identified yet."
                                                   : hw.summary().c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));

                        LauncherSettingLabel("Performance mode",
                                             "Auto follows the benchmark. Anything else is your call "
                                             "and the benchmark stops overriding it.", uiScale);
                        {
                            const char* modes[] = {"Auto", "High", "Balanced", "Low"};
                            int current = 0;
                            for (int i = 0; i < IM_ARRAYSIZE(modes); ++i) {
                                if (prefs.performanceMode == modes[i]) { current = i; break; }
                            }
                            ImGui::SetNextItemWidth(controlW);
                            if (ImGui::Combo("##PerformanceMode", &current, modes,
                                             IM_ARRAYSIZE(modes))) {
                                prefs.performanceMode = modes[current];
                                prefsDirty = true;
                                syncGlassBlurToHardwareTier();
                            }
                        }
                        ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));

                        LauncherSettingLabel("Measured performance",
                                             "A short offscreen render probe. Takes about a second.",
                                             uiScale);
                        if (ImGui::Button("Run benchmark", ImVec2(controlW, 0.0f))) {
                            const HP::BenchmarkResult r = HP::RunBenchmark();
                            if (r.valid) {
                                prefs.hardwareTier = HP::ToString(r.tier);
                                prefs.benchFullscreenPassMs = r.fullscreenPassMs;
                                prefs.benchBlurPassMs = r.blurPassMs;
                                prefs.benchDrawCallBatchMs = r.drawCallBatchMs;
                                prefs.lowSpecPromptAnswered = true;
                                prefsDirty = true;
                                syncGlassBlurToHardwareTier();
                            }
                        }
                        if (prefs.benchFullscreenPassMs > 0.0f) {
                            ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                                   ImGui::GetContentRegionAvail().x);
                            ImGui::Text("Measured %s: fullscreen pass %.2f ms, blur %.2f ms, "
                                        "2000 draws %.2f ms.",
                                        prefs.hardwareTier.empty() ? "?" : prefs.hardwareTier.c_str(),
                                        prefs.benchFullscreenPassMs,
                                        prefs.benchBlurPassMs,
                                        prefs.benchDrawCallBatchMs);
                            ImGui::PopTextWrapPos();
                            ImGui::PopStyleColor();
                        } else {
                            const std::string reason = HP::LowEndReason();
                            if (!reason.empty()) {
                                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                                       ImGui::GetContentRegionAvail().x);
                                ImGui::Text("Not measured yet. Worth running: %s.", reason.c_str());
                                ImGui::PopTextWrapPos();
                                ImGui::PopStyleColor();
                            }
                        }
                        ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));
                    }

                    sectionHeading("Frame pacing",
                                   "Editor-only. The player's own vsync and frame cap live in the "
                                   "project's Graphics settings.");
                    checkboxSetting("Editor vertical sync",
                                    "On is usually smoother: a free-running editor makes the driver "
                                    "block unevenly on swap, which reads as stutter.",
                                    &prefs.editorVSync);
                    checkboxSetting("Cap editor frame rate",
                                    "Useful on a laptop, or when the editor is sharing the GPU with "
                                    "something else.",
                                    &prefs.editorFpsCapEnabled);
                    if (prefs.editorFpsCapEnabled) {
                        LauncherSettingLabel("Frame rate cap", "Frames per second.", uiScale);
                        ImGui::SetNextItemWidth(controlW);
                        if (ImGui::SliderFloat("##FpsCap", &prefs.editorFpsCap, 24.0f, 480.0f, "%.0f FPS")) {
                            prefsDirty = true;
                        }
                        ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));
                    }

                    sectionHeading("Script watching",
                                   "How eagerly the editor looks for changed ModuCPP sources.");
                    checkboxSetting("Compile scripts automatically",
                                    "Off means scripts only build on save, on the Compile button, or "
                                    "during a project build.",
                                    &prefs.scriptAutoCompile);
                    if (prefs.scriptAutoCompile) {
                        LauncherSettingLabel("Change check interval",
                                             "How often modified scripts are noticed.", uiScale);
                        ImGui::SetNextItemWidth(controlW);
                        if (ImGui::SliderFloat("##AutoCompileInterval", &prefs.scriptAutoCompileInterval,
                                               0.1f, 5.0f, "%.1f s")) {
                            prefsDirty = true;
                        }
                        ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));

                        LauncherSettingLabel("Full rescan interval",
                                             "How often the Scripts tree is re-walked for new files. "
                                             "Raise this on a very large project.", uiScale);
                        ImGui::SetNextItemWidth(controlW);
                        if (ImGui::SliderFloat("##ScanInterval", &prefs.scriptDirectoryScanInterval,
                                               1.0f, 60.0f, "%.0f s")) {
                            prefsDirty = true;
                        }
                        ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));
                    }
                    break;
                }
                case 4: {
                    sectionHeading("New project defaults",
                                   "What the Empty preset starts from, and what gets stamped into "
                                   "every project you create.");
                    LauncherSettingLabel("Default pipeline",
                                         "3D, 2.5D or 2D. Only affects the Empty preset; the other "
                                         "presets pick their own.", uiScale);
                    {
                        const char* labels[] = {"3D Pipeline", "2.5D Pipeline", "2D Pipeline"};
                        int current = ImClamp(prefs.defaultPipeline, 0, 2);
                        ImGui::SetNextItemWidth(controlW);
                        if (ImGui::Combo("##DefaultPipeline", &current, labels, 3)) {
                            prefs.defaultPipeline = current;
                            prefsDirty = true;
                        }
                    }
                    ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));

                    LauncherSettingLabel("Default renderer",
                                         "Vulkan needs its pipeline ModuPAK installed and is still "
                                         "experimental.", uiScale);
                    {
                        const char* labels[] = {"OpenGL", "Vulkan"};
                        int current = ImClamp(prefs.defaultRenderer, 0, 1);
                        ImGui::SetNextItemWidth(controlW);
                        if (ImGui::Combo("##DefaultRenderer", &current, labels, 2)) {
                            prefs.defaultRenderer = current;
                            prefsDirty = true;
                        }
                    }
                    ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));

                    LauncherSettingLabel("Company name",
                                         "Written into every new project's player settings, and used "
                                         "for the save-data folder.", uiScale);
                    {
                        char buffer[128];
                        std::snprintf(buffer, sizeof(buffer), "%s", prefs.defaultCompanyName.c_str());
                        ImGui::SetNextItemWidth(controlW);
                        if (ImGui::InputText("##CompanyName", buffer, sizeof(buffer))) {
                            prefs.defaultCompanyName = buffer;
                            prefsDirty = true;
                        }
                    }
                    ImGui::Dummy(ImVec2(0.0f, 9.0f * uiScale));

                    checkboxSetting("Install the default ModuPAK set",
                                    "Adds every package marked as default in the ModuPAK tab to each "
                                    "new project.",
                                    &prefs.applyDefaultPackages);
                    ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextDim);
                    ImGui::Text("%zu package%s currently in the default set.",
                                prefs.defaultPackageIds.size(),
                                prefs.defaultPackageIds.size() == 1 ? "" : "s");
                    ImGui::PopStyleColor();
                    break;
                }
                case 5: {
                    sectionHeading("Editor feedback",
                                   "The short sounds the editor plays for its own actions. Nothing "
                                   "here touches audio in your game.");
                    checkboxSetting("Enable feedback sounds",
                                    "Master switch for every category below.",
                                    &prefs.feedbackSounds);
                    ImGui::BeginDisabled(!prefs.feedbackSounds);
                    checkboxSetting("Clicks and selections",
                                    "Buttons, tabs, project cards.", &prefs.feedbackClickSounds);
                    checkboxSetting("Errors and warnings",
                                    "Failed compiles, invalid actions.", &prefs.feedbackErrorSounds);
                    checkboxSetting("Notifications",
                                    "Package installs, info popups, the startup chime.",
                                    &prefs.feedbackOtherSounds);
                    ImGui::EndDisabled();
                    break;
                }
                case 6: {
                    sectionHeading("Editor language",
                                   "Menus, panels and dialogs. This is a user preference and is never "
                                   "written into a project.");
                    {
                        const auto& languages = Modularity::Loc::Languages();
                        std::vector<const char*> labels;
                        std::vector<std::string> ids;
                        int current = 0;
                        for (size_t i = 0; i < languages.size(); ++i) {
                            labels.push_back(languages[i].displayName.c_str());
                            ids.push_back(languages[i].id);
                            if (languages[i].id == projectManager.editorLanguage) {
                                current = static_cast<int>(i);
                            }
                        }
                        if (!labels.empty()) {
                            ImGui::SetNextItemWidth(controlW);
                            if (ImGui::Combo("##EditorLanguage", &current, labels.data(),
                                             static_cast<int>(labels.size()))) {
                                projectManager.editorLanguage = ids[current];
                                Modularity::Loc::SetLanguage(projectManager.editorLanguage);
                                prefsDirty = true;
                            }
                        }
                    }
                    ImGui::Dummy(ImVec2(0.0f, 12.0f * uiScale));

                    sectionHeading("ModuCPP syntax language",
                                   "Which localized spelling of the ModuCPP keywords new projects are "
                                   "authored in. A project can still override this for itself.");
                    {
                        const auto& languages = ModuCPPLang::Languages();
                        std::vector<const char*> labels;
                        std::vector<std::string> ids;
                        int current = 0;
                        for (size_t i = 0; i < languages.size(); ++i) {
                            labels.push_back(languages[i].displayName.c_str());
                            ids.push_back(languages[i].id);
                            if (languages[i].id == projectManager.moduCppDefaultLanguage) {
                                current = static_cast<int>(i);
                            }
                        }
                        if (!labels.empty()) {
                            ImGui::SetNextItemWidth(controlW);
                            if (ImGui::Combo("##ModuCPPLanguage", &current, labels.data(),
                                             static_cast<int>(labels.size()))) {
                                projectManager.moduCppDefaultLanguage = ids[current];
                                prefsDirty = true;
                            }
                        }
                    }
                    break;
                }
                case 7:
                default: {
                    sectionHeading("Default project location",
                                   "Where the New Project dialog starts, and what the Browse Projects "
                                   "Folder button opens.");
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * uiScale, 7.0f * uiScale));
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputText("##DefaultProjectLocation",
                                     projectManager.defaultProjectLocation,
                                     sizeof(projectManager.defaultProjectLocation));
                    ImGui::PopStyleVar();
                    ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));

                    if (LauncherButton("Apply Location", LauncherButtonKind::Primary,
                                       ImVec2(150.0f * uiScale, 30.0f * uiScale))) {
                        const std::string trimmed = TrimCopy(projectManager.defaultProjectLocation);
                        if (trimmed.empty()) {
                            settingsStatus = "Default project location cannot be empty.";
                            settingsStatusError = true;
                        } else {
                            std::error_code ec;
                            fs::create_directories(trimmed, ec);
                            if (ec) {
                                settingsStatus = "Failed to create folder: " + ec.message();
                                settingsStatusError = true;
                            } else {
                                std::snprintf(projectManager.defaultProjectLocation,
                                              sizeof(projectManager.defaultProjectLocation),
                                              "%s", trimmed.c_str());
                                std::snprintf(projectManager.newProjectLocation,
                                              sizeof(projectManager.newProjectLocation),
                                              "%s", projectManager.defaultProjectLocation);
                                prefsDirty = true;
                                settingsStatus = "Saved.";
                                settingsStatusError = false;
                            }
                        }
                    }
                    ImGui::SameLine();
                    if (LauncherButton("Use Home Default", LauncherButtonKind::Neutral,
                                       ImVec2(170.0f * uiScale, 30.0f * uiScale))) {
#if defined(__ANDROID__)
                        std::string fallback;
                        const std::string docs = Modularity::AndroidRuntime::GetExternalDocumentsPath();
                        if (!docs.empty() && Modularity::AndroidRuntime::HasAllFilesAccess()) {
                            fallback = (fs::path(docs) / "ModularityProjects").string();
                        } else if (const char* dataPath = Modularity::AndroidRuntime::GetInternalDataPath()) {
                            fallback = (fs::path(dataPath) / "ModularityProjects").string();
                        } else {
                            fallback = "/data/local/tmp/ModularityProjects";
                        }
#elif defined(_WIN32)
                        const char* userProfile = std::getenv("USERPROFILE");
                        const std::string fallback = userProfile && *userProfile
                            ? (fs::path(userProfile) / "Documents" / "ModularityProjects").string()
                            : (fs::current_path() / "Projects").string();
#else
                        const char* home = std::getenv("HOME");
                        const std::string fallback = home && *home
                            ? (fs::path(home) / "ModularityProjects").string()
                            : (fs::current_path() / "Projects").string();
#endif
                        std::snprintf(projectManager.defaultProjectLocation,
                                      sizeof(projectManager.defaultProjectLocation),
                                      "%s", fallback.c_str());
                        std::snprintf(projectManager.newProjectLocation,
                                      sizeof(projectManager.newProjectLocation),
                                      "%s", projectManager.defaultProjectLocation);
                        prefsDirty = true;
                        settingsStatus = "Reset to home default.";
                        settingsStatusError = false;
                    }
                    ImGui::SameLine();
                    if (LauncherButton("Open Folder", LauncherButtonKind::Neutral,
                                       ImVec2(120.0f * uiScale, 30.0f * uiScale))) {
                        openInFileManager(fs::path(projectManager.defaultProjectLocation));
                    }

                    ImGui::Dummy(ImVec2(0.0f, 14.0f * uiScale));
                    sectionHeading("Engine locations", "Read-only, shown so you can find them.");
                    LauncherDetailRow("Preferences file",
                                      (projectManager.appDataPath / "launcher_settings.modu").string(),
                                      uiScale);
                    LauncherDetailRow("Recent projects list",
                                      (projectManager.appDataPath / "recent_projects.txt").string(),
                                      uiScale);
                    LauncherDetailRow("Global ModuPAK store",
                                      packageManager.globalPackagesRoot().string(), uiScale);
                    LauncherDetailRow("Template projects",
                                      GetTemplateProjectsRoot().string(), uiScale);
                    LauncherDetailRow("Engine version", "ModuEngine " + engineVersion, uiScale);

#ifdef __ANDROID__
                    ImGui::Dummy(ImVec2(0.0f, 14.0f * uiScale));
                    sectionHeading("File access",
                                   "Android keeps projects in private app storage until shared "
                                   "storage access is granted.");
                    {
                        const bool hasAccess = Modularity::AndroidRuntime::HasAllFilesAccess();
                        const std::string docsRoot = Modularity::AndroidRuntime::GetExternalDocumentsPath();
                        const std::string docsProjects = docsRoot.empty()
                            ? std::string()
                            : (fs::path(docsRoot) / "ModularityProjects").string();
                        if (hasAccess) {
                            ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kGood);
                            ImGui::TextUnformatted("Granted - projects can live in shared storage.");
                            ImGui::PopStyleColor();
                            if (!docsProjects.empty()) {
                                ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
                                if (LauncherButton("Use Documents Folder", LauncherButtonKind::Neutral,
                                                   ImVec2(230.0f * uiScale, 30.0f * uiScale))) {
                                    std::error_code ec;
                                    fs::create_directories(docsProjects, ec);
                                    if (ec) {
                                        settingsStatus = "Failed to create folder: " + ec.message();
                                        settingsStatusError = true;
                                    } else {
                                        std::snprintf(projectManager.defaultProjectLocation,
                                                      sizeof(projectManager.defaultProjectLocation),
                                                      "%s", docsProjects.c_str());
                                        std::snprintf(projectManager.newProjectLocation,
                                                      sizeof(projectManager.newProjectLocation),
                                                      "%s", projectManager.defaultProjectLocation);
                                        prefsDirty = true;
                                        settingsStatus = "Projects will be stored in Documents/ModularityProjects.";
                                        settingsStatusError = false;
                                    }
                                }
                            }
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                            ImGui::TextUnformatted("Not granted. Projects are kept in private app storage,");
                            ImGui::TextUnformatted("which other apps and your file manager can't see.");
                            ImGui::PopStyleColor();
                            ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
                            if (LauncherButton("Grant File Access", LauncherButtonKind::Primary,
                                               ImVec2(230.0f * uiScale, 30.0f * uiScale))) {
                                std::string reqErr;
                                if (Modularity::AndroidRuntime::RequestAllFilesAccess(reqErr)) {
                                    settingsStatus = "Grant access in the settings screen, then return here.";
                                    settingsStatusError = false;
                                } else {
                                    settingsStatus = reqErr.empty()
                                        ? std::string("Could not open the storage permission screen.")
                                        : reqErr;
                                    settingsStatusError = true;
                                }
                            }
                        }
                    }
#endif
                    break;
                }
            }

            ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
            if (!settingsStatus.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      settingsStatusError ? LauncherSkin::kBad : LauncherSkin::kGood);
                ImGui::TextUnformatted(settingsStatus.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, LauncherSkin::kTextFaint);
                ImGui::TextUnformatted("Changes apply as you make them and are saved automatically.");
                ImGui::PopStyleColor();
            }
            const float saveW = 130.0f * uiScale;
            const float saveMargin = 4.0f * uiScale;
            if (ImGui::GetContentRegionAvail().x > saveW + 12.0f * uiScale) {
                ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                saveW - saveMargin);
                if (LauncherButton("Save Settings", LauncherButtonKind::Neutral,
                                   ImVec2(saveW, 26.0f * uiScale))) {
                    prefsDirty = true;
                    settingsStatus = "Saved.";
                    settingsStatusError = false;
                }
            }
            endGroupPanel();

            if (prefsDirty) {
                applyGlobalEditorPreferences(true);
                projectManager.saveLauncherSettings();
            }
        };

        if (launcherSection == 0) {
            const ImVec2 projectsRoot = ImGui::GetCursorPos();
            const ImVec2 projectsAvail = ImGui::GetContentRegionAvail();
            ImDrawList* projectsDrawList = ImGui::GetWindowDrawList();
            const ImVec2 projectsClipMin = ImGui::GetCursorScreenPos();
            const ImVec2 projectsClipMax(projectsClipMin.x + projectsAvail.x, projectsClipMin.y + projectsAvail.y);
            projectsDrawList->PushClipRect(projectsClipMin, projectsClipMax, true);

            auto beginProjectsSubpanel = [&](const char* id, float offsetUnits) {
                const float eased = EaseOutCubic(projectsPanelVisual);
                const float offsetX = (offsetUnits - eased) * projectsAvail.x;
                ImGui::SetCursorPos(ImVec2(projectsRoot.x + offsetX, projectsRoot.y));
                ImGui::BeginChild(id, projectsAvail, false,
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
            };

            beginProjectsSubpanel("ProjectsListPanel", 0.0f);
            renderProjectsView();
            ImGui::EndChild();

            beginProjectsSubpanel("ProjectsNewPanel", 1.0f);
            renderNewProjectView();
            ImGui::EndChild();

            projectsDrawList->PopClipRect();
        } else if (launcherSection == 1) {
            renderPackagesView();
        } else {
            renderSettingsView();
        }

        ImGui::EndChild();      // LauncherActiveSection
        ImGui::PopStyleColor(); // transparent ChildBg (covered section + subpanels)
        ImGui::PopStyleVar();   // section alpha
        ImGui::PopStyleVar();   // content alpha

        {
            const ImVec2 statusMin(windowPos.x, windowPos.y + windowSize.y - statusBarHeight);
            const ImVec2 statusMax(windowPos.x + windowSize.x, windowPos.y + windowSize.y);
            LauncherShadeRect(drawList, statusMin, statusMax, LauncherSkin::kChrome,
                              ImGuiShadeClass_MenuBar, ImGuiShadeState_Normal, 0.0f,
                              contentAlpha * transitionAlpha);
            drawList->AddLine(ImVec2(statusMin.x, statusMin.y + 0.5f),
                              ImVec2(statusMax.x, statusMin.y + 0.5f),
                              LauncherSkin::U32(LauncherSkin::kSeam,
                                                contentAlpha * transitionAlpha),
                              1.0f);

            const float statusFontSize = baseFontSize * 0.84f;
            const float cellPad = 14.0f * uiScale;
            const float textY = statusMin.y + (statusBarHeight - statusFontSize) * 0.5f;
            const float statusAlpha = contentAlpha * transitionAlpha;
            float cellX = statusMin.x + cellPad;

            auto statusCell = [&](const std::string& text, const ImVec4& colour) {
                if (text.empty()) return;
                const float w = ImGui::GetFont()->CalcTextSizeA(statusFontSize, FLT_MAX, 0.0f,
                                                                text.c_str()).x;
                if (cellX + w > statusMax.x - cellPad) return;
                drawList->AddText(ImGui::GetFont(), statusFontSize, ImVec2(cellX, textY),
                                  LauncherSkin::U32(colour, statusAlpha), text.c_str());
                cellX += w + cellPad;
                drawList->AddLine(ImVec2(cellX - cellPad * 0.5f, statusMin.y + 6.0f * uiScale),
                                  ImVec2(cellX - cellPad * 0.5f, statusMax.y - 6.0f * uiScale),
                                  LauncherSkin::U32(LauncherSkin::kSeam, statusAlpha), 1.0f);
            };

            statusCell("ModuEngine " + engineVersion, LauncherSkin::kTextDim);
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%zu project%s tracked",
                              projectManager.recentProjects.size(),
                              projectManager.recentProjects.size() == 1 ? "" : "s");
                statusCell(buf, LauncherSkin::kTextDim);
            }
            {
                const size_t defaults = projectManager.preferences.defaultPackageIds.size();
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%zu default ModuPAK%s",
                              defaults, defaults == 1 ? "" : "s");
                statusCell(buf, defaults > 0 ? LauncherSkin::kAccentHi : LauncherSkin::kTextFaint);
            }
            statusCell(std::string("Projects: ") + projectManager.defaultProjectLocation,
                       LauncherSkin::kTextFaint);

            const char* activity = launcherBusy ? "Loading project..." : "Ready";
            const float activityW = ImGui::GetFont()->CalcTextSizeA(statusFontSize, FLT_MAX, 0.0f,
                                                                    activity).x;
            drawList->AddText(ImGui::GetFont(), statusFontSize,
                              ImVec2(statusMax.x - cellPad - activityW, textY),
                              LauncherSkin::U32(launcherBusy ? LauncherSkin::kWarn
                                                             : LauncherSkin::kGood,
                                                statusAlpha),
                              activity);
        }

        if (!launcherIntroFinished) {
            ImGui::PopItemFlag();
        }
        ImGui::SetWindowFontScale(1.0f);

        if (!launcherIntroFinished && introState.textAlpha > 0.001f) {
            ImDrawList* overlay = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
            const char* title = "Modularity";
            ImFont* font = ImGui::GetFont();
            const float baseFs = ImGui::GetFontSize();
            const float centerFontSize = baseFs * 2.62f;
            const float headerFontSize = titleFontSize;
            const ImVec4 baseCol = ImVec4(0.95f, 0.96f, 0.98f, 1.0f);

            // Measure widths at both sizes
            float centerTotalWidth = 0.0f;
            float headerTotalWidth = 0.0f;
            for (const char* c = title; *c; ++c) {
                char letter[2] = { *c, 0 };
                centerTotalWidth += font->CalcTextSizeA(centerFontSize, FLT_MAX, 0.0f, letter).x;
                headerTotalWidth += font->CalcTextSizeA(headerFontSize, FLT_MAX, 0.0f, letter).x;
            }

            // Center layout: logo sits to the left of the text, group is centered.
            const float centerLogoSize = centerFontSize * 1.18f;
            const float centerLogoGap  = 18.0f * uiScale;
            const float centerGroupW   = centerLogoSize + centerLogoGap + centerTotalWidth;
            const ImVec2 centerOrigin(displaySize.x * 0.5f - centerGroupW * 0.5f,
                                      displaySize.y * 0.40f);
            const ImVec2 centerLogoMin(centerOrigin.x,
                                       centerOrigin.y + (centerFontSize - centerLogoSize) * 0.5f);
            const ImVec2 centerTextPos (centerOrigin.x + centerLogoSize + centerLogoGap,
                                        centerOrigin.y);

            // Logo: fades in during fadeIn phase (before letters pop in), then drifts to header pos.
            const float logoFadeT = ImClamp(introState.elapsed / std::max(0.0001f, introTimings.fadeIn), 0.0f, 1.0f);
            const float logoFadeAlpha = EaseOutCubic(logoFadeT);
            const float driftEase = introState.driftEase;
            if (logoTexId != static_cast<ImTextureID>(0)) {
                const float logoSize = ImLerp(centerLogoSize, headerLogoSize, driftEase);
                const ImVec2 lMin(
                    ImLerp(centerLogoMin.x, headerLogoPos.x, driftEase),
                    ImLerp(centerLogoMin.y, headerLogoPos.y, driftEase));
                const ImVec2 lMax(lMin.x + logoSize, lMin.y + logoSize);
                const float lAlpha = logoFadeAlpha * ImLerp(1.0f, 1.0f, driftEase);
                overlay->AddImage(logoTexId, lMin, lMax, logoUvMin, logoUvMax,
                                  ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, lAlpha)));
            }

            // Letters: each pops in beside the logo with a slight delay, then drifts to header text.
            float centerAdvanceX = 0.0f;
            float headerAdvanceX = 0.0f;
            int index = 0;
            const int totalLetters = static_cast<int>(std::strlen(title));
            const float popDelayStep = 0.055f;
            const float driftDelayStep = 0.022f;
            for (const char* c = title; *c; ++c, ++index) {
                char letter[2] = { *c, 0 };
                const float popDelay = (totalLetters > 1) ? (popDelayStep * (float)index) : 0.0f;
                const float popInput = ImClamp(
                    (introState.elapsed - introTimings.fadeIn - popDelay) / std::max(0.0001f, introTimings.popIn),
                    0.0f, 1.0f);
                const float popEase  = EaseOutBack(popInput);
                const float popAlpha = EaseOutCubic(popInput);

                const float driftDelay = (totalLetters > 1) ? (driftDelayStep * (float)index) : 0.0f;
                const float driftInput = ImClamp(
                    (introState.driftT - driftDelay) / std::max(0.0001f, 1.0f - driftDelay),
                    0.0f, 1.0f);
                const float driftL = EaseInOutCubic(driftInput);

                const float centerWidth = font->CalcTextSizeA(centerFontSize, FLT_MAX, 0.0f, letter).x;
                const float headerWidth = font->CalcTextSizeA(headerFontSize, FLT_MAX, 0.0f, letter).x;
                const ImVec2 centerLetterPos(centerTextPos.x + centerAdvanceX, centerTextPos.y);
                const ImVec2 headerLetterPos(headerTextPos.x + headerAdvanceX, headerTextPos.y);

                const float popScale = std::max(0.05f, 0.34f + 0.66f * popEase);
                const float popSize  = centerFontSize * popScale;
                const ImVec2 spawnPos(centerLetterPos.x, centerLetterPos.y + (1.0f - popAlpha) * 22.0f * uiScale);
                const ImVec2 popPos(
                    ImLerp(spawnPos.x, centerLetterPos.x, popAlpha),
                    ImLerp(spawnPos.y, centerLetterPos.y, popAlpha));

                const float letterSize = ImLerp(popSize, headerFontSize, driftL);
                const ImVec2 letterPos(
                    ImLerp(popPos.x, headerLetterPos.x, driftL),
                    ImLerp(popPos.y, headerLetterPos.y, driftL));
                const float letterAlpha = introState.textAlpha * popAlpha * ImLerp(0.92f, 1.0f, driftL);
                const ImU32 textCol = ImGui::GetColorU32(ImVec4(baseCol.x, baseCol.y, baseCol.z, letterAlpha));
                overlay->AddText(font, letterSize, letterPos, textCol, letter);

                centerAdvanceX += centerWidth;
                headerAdvanceX += headerWidth;
            }
        }

        if (loadingScreenT > 0.0001f) {
            ImVec2 focus = launcherTransitionFocus;
            if (focus.x <= 0.0f && focus.y <= 0.0f) {
                focus = ImVec2(windowPos.x + windowSize.x * 0.5f, windowPos.y + windowSize.y * 0.5f);
            }

            ImTextureID zoomTexId = static_cast<ImTextureID>(0);
            int zoomTexW = 0;
            int zoomTexH = 0;
            if (!launcherLoadingPreviewPath.empty()) {
                const fs::path previewP = launcherLoadingPreviewPath;
                if (fs::exists(previewP)) {
                    if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
                        zoomTexId = vulkanRenderer->getOrCreateUIImage(previewP.string(),
                                                                       &zoomTexW, &zoomTexH);
                    } else if (!usingVulkan()) {
                        if (Texture* tex = renderer.getTexture(previewP.string())) {
                            zoomTexId = (ImTextureID)(intptr_t)tex->GetID();
                            zoomTexW = tex->GetWidth();
                            zoomTexH = tex->GetHeight();
                        }
                    }
                }
            }

            const float cardW0 = 1100.0f * uiScale;
            const float cardH0 = 92.0f   * uiScale;
            const float distLeft   = focus.x - windowPos.x;
            const float distRight  = (windowPos.x + windowSize.x) - focus.x;
            const float distTop    = focus.y - windowPos.y;
            const float distBottom = (windowPos.y + windowSize.y) - focus.y;
            const float targetHalfW = std::max(distLeft, distRight) + 120.0f * uiScale;
            const float targetHalfH = std::max(distTop, distBottom)  + 120.0f * uiScale;
            const float curW = ImLerp(cardW0, targetHalfW * 2.0f, loadingScreenT);
            const float curH = ImLerp(cardH0, targetHalfH * 2.0f, loadingScreenT);
            const ImVec2 zoomMin(focus.x - curW * 0.5f, focus.y - curH * 0.5f);
            const ImVec2 zoomMax(focus.x + curW * 0.5f, focus.y + curH * 0.5f);
            const float radius    = ImLerp(11.0f, 0.0f, loadingScreenT) * uiScale;
            const float coreAlpha = ImLerp(0.0f, 1.0f, loadingScreenT);

            if (zoomTexId != static_cast<ImTextureID>(0)) {
                DrawImageCover(drawList, zoomTexId,
                               zoomMin, zoomMax,
                               zoomTexW, zoomTexH,
                               ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, coreAlpha)),
                               radius);
                // Dark scrim ramps up as it covers the screen so it transitions into
                // the loading backdrop instead of just sitting as a bright thumbnail.
                const float scrimAlpha = ImLerp(0.0f, 0.62f, loadingScreenT);
                drawList->AddRectFilled(zoomMin, zoomMax,
                                        ImGui::GetColorU32(ImVec4(0.04f, 0.05f, 0.08f, scrimAlpha)),
                                        radius);
            } else {
                drawList->AddRectFilled(zoomMin, zoomMax,
                                        ImGui::GetColorU32(ImVec4(0.05f, 0.07f, 0.10f, coreAlpha)),
                                        radius);
            }
            // Thin accent border that fades quickly so the rect dissolves into the bg.
            const float borderAlpha = ImLerp(0.85f, 0.0f, ImClamp(loadingScreenT * 1.6f, 0.0f, 1.0f));
            if (borderAlpha > 0.001f) {
                drawList->AddRect(zoomMin, zoomMax,
                                  ImGui::GetColorU32(ImVec4(0.42f, 0.72f, 1.0f, borderAlpha)),
                                  radius, 0, 1.8f * uiScale);
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(5);

    if (launcherTransitionActive && transitionT >= 1.0f) {
        launcherTransitionActive = false;
        if (launcherTransitionPendingHide || (!projectLoadInProgress && !sceneLoadInProgress)) {
            launcherTransitionPendingHide = false;
            showLauncher = false;
        }
    } else if (!launcherTransitionActive && launcherTransitionPendingHide &&
               !projectLoadInProgress && !sceneLoadInProgress) {
        launcherTransitionPendingHide = false;
        showLauncher = false;
    }

    if (projectManager.showOpenProjectDialog)
        renderOpenProjectDialog();

    if (projectLoadInProgress || sceneLoadInProgress) {
        const float elapsed = static_cast<float>(glfwGetTime() - projectLoadStartTime);
        // Wait for the zoom to have made meaningful progress before fading the loading
        // card in, so we don't get a snappy double-overlay during the first frames.
        const float introT  = ImClamp((elapsed - 0.32f) / 0.32f, 0.0f, 1.0f);
        const float introEase = EaseOutCubic(introT);

        if (!launcherWindSoundActive && elapsed > 1.4f) {
            if (playEditorFeedbackOneShot("Resources/Sounds/Wind Sound Load Project.mp3",
                                          0.55f, EditorFeedbackSoundCategory::Boot)) {
                launcherWindSoundActive = true;
            }
        }

        ImGuiIO& io2 = ImGui::GetIO();
        ImGuiViewport* overlayViewport = ImGui::GetMainViewport();
        const ImVec2 overlayPos = overlayViewport ? overlayViewport->Pos : ImVec2(0.0f, 0.0f);
        const ImVec2 overlaySize = overlayViewport ? overlayViewport->Size : io2.DisplaySize;
        if (overlayViewport) {
            ImGui::SetNextWindowViewport(overlayViewport->ID);
        }
        ImGui::SetNextWindowPos(overlayPos);
        ImGui::SetNextWindowSize(overlaySize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.08f, 0.42f * introEase));
        ImGui::Begin("ProjectLoadOverlay", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoInputs);
        ImGui::End();
        ImGui::PopStyleColor();

        ImVec2 center = overlayViewport ? overlayViewport->GetCenter()
                                        : ImVec2(io2.DisplaySize.x * 0.5f, io2.DisplaySize.y * 0.5f);
        if (overlayViewport) {
            ImGui::SetNextWindowViewport(overlayViewport->ID);
        }
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(480.0f, 184.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, introEase);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.105f, 0.118f, 0.155f, 0.97f));
        ImGui::Begin("ProjectLoadCard", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);

        std::string headline;
        if (sceneLoadInProgress) {
            headline = "Loading scene...";
        } else if (!launcherTransitionProjectName.empty()) {
            headline = "Loading " + launcherTransitionProjectName + "...";
        } else {
            headline = "Loading project...";
        }
        ImGui::TextColored(ImVec4(0.94f, 0.96f, 1.0f, 1.0f), "%s", headline.c_str());
        ImGui::Spacing();
        if (sceneLoadInProgress && !sceneLoadStatus.empty()) {
            ImGui::TextDisabled("%s", sceneLoadStatus.c_str());
        } else if (!projectLoadPath.empty()) {
            ImGui::TextDisabled("%s", projectLoadPath.c_str());
        }
        ImGui::Spacing();
        if (sceneLoadInProgress) {
            ImGui::ProgressCircle("##project_load_circle", 16.0f, 4.0f, sceneLoadProgress,
                                  ImGui::GetColorU32(ImGuiCol_ButtonHovered),
                                  ImGui::GetColorU32(ImGuiCol_Button));
            ImGui::SameLine();
            ImGui::BufferingBar("##project_load_bar", sceneLoadProgress,
                                ImVec2(ImGui::GetContentRegionAvail().x - 40.0f, 8.0f),
                                ImGui::GetColorU32(ImGuiCol_Button),
                                ImGui::GetColorU32(ImGuiCol_ButtonHovered));
        } else {
            ImGui::Spinner("##project_load_spinner", 16.0f, 4, ImGui::GetColorU32(ImGuiCol_ButtonHovered));
            ImGui::SameLine();
            ImGui::BufferingBar("##project_load_bar", std::fmod(elapsed * 0.25f, 1.0f),
                                ImVec2(ImGui::GetContentRegionAvail().x - 40.0f, 8.0f),
                                ImGui::GetColorU32(ImGuiCol_Button),
                                ImGui::GetColorU32(ImGuiCol_ButtonHovered));
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    } else if (launcherWindSoundActive) {
        launcherWindSoundActive = false;
    }
}
#pragma endregion

#pragma region New Project Dialog
void Engine::renderNewProjectDialog() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImVec2 center = mainViewport ? mainViewport->GetCenter()
                                 : ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

    if (mainViewport) {
        ImGui::SetNextWindowViewport(mainViewport->ID);
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(1040, 720), ImGuiCond_Appearing);

    if (ImGui::Begin("New Project", &projectManager.showNewProjectDialog,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        static int templateCategory = 0; // 0 = Blank Project, 1 = Template Projects
        static char templateSearch[128] = "";
        const std::vector<LauncherTemplateEntry> templates = GatherTemplateEntries();
        const std::vector<LauncherTemplateEntry> builtInPresets = BuiltInProjectPresets();
        const fs::path templatesRoot = GetTemplateProjectsRoot();
        const std::string templateFilter = TrimCopy(templateSearch);

        LauncherTemplateEntry blankEntry = builtInPresets.empty() ? LauncherTemplateEntry{} : builtInPresets.front();

        auto selectTemplateEntry = [&](const LauncherTemplateEntry& entry) {
            if (entry.isBlankPreset) {
                projectManager.newProjectTemplatePath.clear();
                projectManager.newProjectTemplateName = entry.displayName;
                projectManager.newProjectPresetId = entry.presetId.empty() ? "empty" : entry.presetId;
            } else {
                projectManager.newProjectTemplatePath = entry.projectRoot.string();
                projectManager.newProjectTemplateName = entry.displayName;
                projectManager.newProjectPresetId.clear();
            }
            projectManager.newProjectPipelineMode = ProjectPipelineToUiIndex(entry.pipeline);
            projectManager.newProjectRendererMode = (entry.rendererBackend == Modularity::GraphicsBackend::Vulkan) ? 1 : 0;
#if !MODULARITY_HAS_VULKAN
            if (projectManager.newProjectRendererMode == 1) {
                projectManager.newProjectRendererMode = 0;
            }
#else
            if (projectManager.newProjectRendererMode == 1 && !hasVulkanPipelinePackage()) {
                projectManager.newProjectRendererMode = 0;
            }
#endif
            projectManager.newProjectImportLastPackages = false;
        };

        if (projectManager.newProjectTemplateName.empty()) {
            projectManager.newProjectTemplateName = blankEntry.displayName;
            projectManager.newProjectPresetId = blankEntry.presetId;
        }

        const LauncherTemplateEntry* selectedTemplate = nullptr;
        if (projectManager.newProjectTemplatePath.empty()) {
            selectedTemplate = &blankEntry;
            for (const auto& preset : builtInPresets) {
                if (preset.presetId == projectManager.newProjectPresetId ||
                    preset.displayName == projectManager.newProjectTemplateName) {
                    selectedTemplate = &preset;
                    break;
                }
            }
        } else {
            for (const auto& t : templates) {
                if (fs::path(projectManager.newProjectTemplatePath) == t.projectRoot) {
                    selectedTemplate = &t;
                    break;
                }
            }
        }
        if (!selectedTemplate) {
            projectManager.newProjectTemplatePath.clear();
            projectManager.newProjectTemplateName = blankEntry.displayName;
            projectManager.newProjectPresetId = blankEntry.presetId;
            selectedTemplate = &blankEntry;
        }
        projectManager.newProjectPipelineMode = ProjectPipelineToUiIndex(selectedTemplate->pipeline);
        projectManager.newProjectRendererMode = (selectedTemplate->rendererBackend == Modularity::GraphicsBackend::Vulkan) ? 1 : 0;
        projectManager.newProjectImportLastPackages = false;
#if !MODULARITY_HAS_VULKAN
        if (projectManager.newProjectRendererMode == 1) {
            projectManager.newProjectRendererMode = 0;
        }
#else
        if (projectManager.newProjectRendererMode == 1 && !hasVulkanPipelinePackage()) {
            projectManager.newProjectRendererMode = 0;
        }
#endif

        auto resolvePreviewTexture = [&](const LauncherTemplateEntry& entry, ImTextureID& outId, int& outW, int& outH) {
            outId = static_cast<ImTextureID>(0);
            outW = 0;
            outH = 0;
            if (entry.previewImage.empty() || !fs::exists(entry.previewImage)) {
                return;
            }
            if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
                outId = vulkanRenderer->getOrCreateUIImage(entry.previewImage.string(), &outW, &outH);
            } else if (!usingVulkan()) {
                if (Texture* templateTex = renderer.getTexture(entry.previewImage.string())) {
                    outId = (ImTextureID)(intptr_t)templateTex->GetID();
                    outW = templateTex->GetWidth();
                    outH = templateTex->GetHeight();
                }
            }
        };

        auto createProjectFromCurrentState = [&]() {
            if (std::strlen(projectManager.newProjectName) == 0) {
                projectManager.errorMessage = "Please enter a project name";
            } else if (std::strlen(projectManager.newProjectLocation) == 0) {
                projectManager.errorMessage = "Please specify a location";
            } else {
                createNewProject(projectManager.newProjectName, projectManager.newProjectLocation);
                if (projectManager.errorMessage.empty()) {
                    projectManager.showNewProjectDialog = false;
                }
            }
        };

        auto renderCategoryButton = [&](const char* label, int category, float width) {
            const bool selected = templateCategory == category;
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.20f, 0.31f, 0.46f, 1.0f)
                                                            : ImVec4(0.13f, 0.16f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? ImVec4(0.24f, 0.36f, 0.53f, 1.0f)
                                                                   : ImVec4(0.18f, 0.22f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.21f, 0.33f, 0.49f, 1.0f));
            if (ImGui::Button(label, ImVec2(width, 0.0f))) {
                templateCategory = category;
                if (templateCategory == 0) {
                    selectTemplateEntry(blankEntry);
                    selectedTemplate = &blankEntry;
                } else if (!templates.empty() && selectedTemplate && selectedTemplate->isBlankPreset) {
                    selectTemplateEntry(templates.front());
                    selectedTemplate = &templates.front();
                }
            }
            ImGui::PopStyleColor(3);
        };

        auto renderTemplateListRow = [&](const LauncherTemplateEntry& entry) {
            const bool selected = selectedTemplate &&
                ((entry.isBlankPreset && selectedTemplate->isBlankPreset &&
                  entry.presetId == selectedTemplate->presetId) ||
                 (!entry.isBlankPreset && !selectedTemplate->isBlankPreset &&
                  selectedTemplate->projectRoot == entry.projectRoot));

            ImGui::PushID(entry.isBlankPreset ? "NewProjectBlankTemplate" : entry.projectRoot.string().c_str());
            const ImVec2 rowPos = ImGui::GetCursorScreenPos();
            const ImVec2 rowSize(ImGui::GetContentRegionAvail().x, 64.0f);
            ImGui::InvisibleButton("TemplateListRow", rowSize);
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) {
                selectTemplateEntry(entry);
                selectedTemplate = &entry;
            }

            ImDrawList* list = ImGui::GetWindowDrawList();
            const ImVec2 rowMax(rowPos.x + rowSize.x, rowPos.y + rowSize.y);
            if (hovered || selected) {
                list->AddRectFilled(rowPos,
                                    rowMax,
                                    ImGui::GetColorU32(selected ? ImVec4(0.14f, 0.19f, 0.27f, 0.96f)
                                                                : ImVec4(0.11f, 0.14f, 0.19f, 0.82f)),
                                    4.0f);
            }
            list->AddRectFilled(ImVec2(rowPos.x, rowPos.y + 6.0f),
                                ImVec2(rowPos.x + (selected ? 3.0f : 2.0f), rowMax.y - 6.0f),
                                ImGui::GetColorU32(selected ? ImVec4(0.42f, 0.72f, 1.0f, 1.0f)
                                                            : ImVec4(0.27f, 0.38f, 0.52f, hovered ? 0.65f : 0.0f)),
                                2.0f);
            list->AddLine(ImVec2(rowPos.x, rowMax.y),
                          ImVec2(rowMax.x, rowMax.y),
                          ImGui::GetColorU32(ImVec4(0.18f, 0.22f, 0.30f, 1.0f)),
                          1.0f);

            const ImVec2 thumbMin(rowPos.x + 10.0f, rowPos.y + 8.0f);
            const ImVec2 thumbMax(thumbMin.x + 82.0f, rowMax.y - 8.0f);
            ImTextureID textureId = static_cast<ImTextureID>(0);
            int textureW = 0;
            int textureH = 0;
            resolvePreviewTexture(entry, textureId, textureW, textureH);
            if (textureId != static_cast<ImTextureID>(0)) {
                DrawImageCover(list, textureId, thumbMin, thumbMax, textureW, textureH, ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 4.0f);
            } else {
                list->AddRectFilled(thumbMin, thumbMax, ImGui::GetColorU32(ImVec4(0.08f, 0.10f, 0.14f, 1.0f)), 4.0f);
                const char* placeholder = entry.isBlankPreset ? "Default" : "Preview";
                const ImVec2 textSize = ImGui::CalcTextSize(placeholder);
                list->AddText(ImVec2(thumbMin.x + ((thumbMax.x - thumbMin.x) - textSize.x) * 0.5f,
                                     thumbMin.y + ((thumbMax.y - thumbMin.y) - textSize.y) * 0.5f),
                              ImGui::GetColorU32(ImVec4(0.63f, 0.69f, 0.77f, 1.0f)),
                              placeholder);
            }

            ImGui::SetCursorScreenPos(ImVec2(thumbMax.x + 12.0f, rowPos.y + 10.0f));
            ImGui::TextColored(ImVec4(0.93f, 0.96f, 1.0f, 1.0f), "%s", entry.displayName.c_str());
            ImGui::SetCursorScreenPos(ImVec2(thumbMax.x + 12.0f, rowPos.y + 30.0f));
            ImGui::TextColored(ImVec4(0.61f, 0.68f, 0.77f, 1.0f), "%s", ProjectPipelineLabel(entry.pipeline));
            ImGui::Dummy(ImVec2(rowSize.x, 2.0f));
            ImGui::PopID();
        };

        static int templateViewMode = 1; // 0 = Grid, 1 = List

        auto renderTemplateViewButton = [&](const char* label, int mode, float width) {
            const bool selected = templateViewMode == mode;
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.20f, 0.31f, 0.46f, 1.0f)
                                                            : ImVec4(0.13f, 0.16f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? ImVec4(0.24f, 0.36f, 0.53f, 1.0f)
                                                                   : ImVec4(0.18f, 0.22f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.21f, 0.33f, 0.49f, 1.0f));
            if (ImGui::Button(label, ImVec2(width, 0.0f))) {
                templateViewMode = mode;
            }
            ImGui::PopStyleColor(3);
        };

        auto renderTemplateGridTile = [&](const LauncherTemplateEntry& entry, float tileWidth) {
            const bool selected = selectedTemplate &&
                ((entry.isBlankPreset && selectedTemplate->isBlankPreset &&
                  entry.presetId == selectedTemplate->presetId) ||
                 (!entry.isBlankPreset && !selectedTemplate->isBlankPreset &&
                  selectedTemplate->projectRoot == entry.projectRoot));

            ImGui::PushID(entry.isBlankPreset ? "NewProjectBlankTemplateGrid" : entry.projectRoot.string().c_str());
            const float tileHeight = 114.0f;
            const float previewHeight = 64.0f;
            const ImVec2 tilePos = ImGui::GetCursorScreenPos();
            const ImVec2 tileSize(tileWidth, tileHeight);
            ImGui::InvisibleButton("TemplateGridTile", tileSize);
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) {
                selectTemplateEntry(entry);
                selectedTemplate = &entry;
            }

            ImDrawList* list = ImGui::GetWindowDrawList();
            const ImVec2 tileMax(tilePos.x + tileSize.x, tilePos.y + tileSize.y);
            const ImVec2 thumbMin(tilePos.x, tilePos.y);
            const ImVec2 thumbMax(tileMax.x, tilePos.y + previewHeight);
            list->AddRectFilled(tilePos,
                                tileMax,
                                ImGui::GetColorU32(selected ? ImVec4(0.13f, 0.18f, 0.25f, 0.96f)
                                                            : ImVec4(0.10f, 0.13f, 0.18f, hovered ? 0.90f : 0.78f)),
                                3.0f);
            list->AddRect(tilePos,
                          tileMax,
                          ImGui::GetColorU32(selected ? ImVec4(0.39f, 0.66f, 0.97f, 1.0f)
                                                      : ImVec4(0.20f, 0.25f, 0.33f, hovered ? 1.0f : 0.85f)),
                          3.0f);
            if (selected || hovered) {
                list->AddRectFilled(ImVec2(tilePos.x, tilePos.y),
                                    ImVec2(tilePos.x + tileSize.x, tilePos.y + 2.0f),
                                    ImGui::GetColorU32(selected ? ImVec4(0.42f, 0.72f, 1.0f, 1.0f)
                                                                : ImVec4(0.28f, 0.42f, 0.60f, 0.85f)));
            }

            ImTextureID textureId = static_cast<ImTextureID>(0);
            int textureW = 0;
            int textureH = 0;
            resolvePreviewTexture(entry, textureId, textureW, textureH);
            if (textureId != static_cast<ImTextureID>(0)) {
                DrawImageCover(list, textureId, thumbMin, thumbMax, textureW, textureH, ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 3.0f);
            } else {
                list->AddRectFilled(thumbMin, thumbMax, ImGui::GetColorU32(ImVec4(0.08f, 0.10f, 0.14f, 1.0f)), 3.0f);
                const char* placeholder = entry.isBlankPreset ? "Default" : "Preview";
                const ImVec2 textSize = ImGui::CalcTextSize(placeholder);
                list->AddText(ImVec2(thumbMin.x + ((thumbMax.x - thumbMin.x) - textSize.x) * 0.5f,
                                     thumbMin.y + ((thumbMax.y - thumbMin.y) - textSize.y) * 0.5f),
                              ImGui::GetColorU32(ImVec4(0.63f, 0.69f, 0.77f, 1.0f)),
                              placeholder);
            }

            ImGui::SetCursorScreenPos(ImVec2(tilePos.x + 8.0f, thumbMax.y + 8.0f));
            ImGui::PushTextWrapPos(tilePos.x + tileSize.x - 8.0f);
            ImGui::TextColored(ImVec4(0.93f, 0.96f, 1.0f, 1.0f), "%s", entry.displayName.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos(ImVec2(tilePos.x + 8.0f, tileMax.y - 22.0f));
            ImGui::TextColored(ImVec4(0.61f, 0.68f, 0.77f, 1.0f), "%s", ProjectPipelineLabel(entry.pipeline));
            ImGui::PopID();
        };

        ImGui::TextColored(ImVec4(0.94f, 0.97f, 1.0f, 1.0f), "Create New Project");
        ImGui::TextColored(ImVec4(0.59f, 0.66f, 0.75f, 1.0f),
                           "Choose a base project, review the preview, then configure the project details below.");
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        renderCategoryButton("Defaults", 0, 130.0f);
        ImGui::SameLine();
        renderCategoryButton("Template Projects", 1, 164.0f);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        const float splitHeight = std::max(340.0f, ImGui::GetContentRegionAvail().y);
        const float leftWidth = ImClamp(ImGui::GetContentRegionAvail().x * 0.56f, 400.0f, 620.0f);

        ImGui::BeginChild("TemplateSelectionPane", ImVec2(leftWidth, splitHeight), false);
        ImGui::TextColored(ImVec4(0.89f, 0.93f, 0.98f, 1.0f), "Templates");
        const float toggleWidth = 112.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                      ImGui::GetWindowWidth() - toggleWidth - ImGui::GetStyle().WindowPadding.x));
        renderTemplateViewButton("Grid", 0, 52.0f);
        ImGui::SameLine(0.0f, 4.0f);
        renderTemplateViewButton("List", 1, 52.0f);
        if (templateCategory == 1) {
            ImGui::TextDisabled("Source: %s", templatesRoot.string().c_str());
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##DialogTemplateSearch", "Search templates", templateSearch, sizeof(templateSearch));
        } else {
            ImGui::TextDisabled("Start from an empty project layout.");
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        ImGui::BeginChild("TemplateSelectionRows", ImVec2(0.0f, 0.0f), false);
        if (templateCategory == 0) {
            if (templateViewMode == 0) {
                int tileIndex = 0;
                const float tileSpacing = 8.0f;
                const float contentWidth = ImGui::GetContentRegionAvail().x;
                const float minTileWidth = 146.0f;
                const int tileColumns = std::max(1, static_cast<int>((contentWidth + tileSpacing) / (minTileWidth + tileSpacing)));
                const float tileWidth = (contentWidth - tileSpacing * static_cast<float>(tileColumns - 1)) / static_cast<float>(tileColumns);
                for (const auto& preset : builtInPresets) {
                    if (tileIndex > 0 && (tileIndex % tileColumns) != 0) {
                        ImGui::SameLine(0.0f, tileSpacing);
                    }
                    renderTemplateGridTile(preset, tileWidth);
                    ++tileIndex;
                }
            } else {
                for (const auto& preset : builtInPresets) {
                    renderTemplateListRow(preset);
                }
            }
        } else {
            bool hadVisible = false;
            int tileIndex = 0;
            const float tileSpacing = 8.0f;
            const float contentWidth = ImGui::GetContentRegionAvail().x;
            const float minTileWidth = 146.0f;
            const int tileColumns = std::max(1, static_cast<int>((contentWidth + tileSpacing) / (minTileWidth + tileSpacing)));
            const float tileWidth = (contentWidth - tileSpacing * static_cast<float>(tileColumns - 1)) / static_cast<float>(tileColumns);
            for (const auto& t : templates) {
                const std::string lowered = TrimCopy(t.displayName);
                std::string loweredSearch = lowered;
                std::transform(loweredSearch.begin(), loweredSearch.end(), loweredSearch.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                std::string searchLower = templateFilter;
                std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (!searchLower.empty() && loweredSearch.find(searchLower) == std::string::npos) {
                    continue;
                }
                if (templateViewMode == 0) {
                    if (tileIndex > 0 && (tileIndex % tileColumns) != 0) {
                        ImGui::SameLine(0.0f, tileSpacing);
                    }
                    renderTemplateGridTile(t, tileWidth);
                    ++tileIndex;
                } else {
                    renderTemplateListRow(t);
                }
                hadVisible = true;
            }
            if (!hadVisible) {
                if (templates.empty()) {
                    ImGui::TextDisabled("No templates found.");
                    ImGui::TextDisabled("Add folders with project.modu to Template-Projects.");
                } else {
                    ImGui::TextDisabled("No templates match your search.");
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("TemplatePreviewPane", ImVec2(0.0f, splitHeight), false);
        ImGui::TextColored(ImVec4(0.89f, 0.93f, 0.98f, 1.0f), "Preview");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        {
            const float previewHeight = std::min(162.0f, ImGui::GetContentRegionAvail().y * 0.26f);
            const ImVec2 previewPos = ImGui::GetCursorScreenPos();
            const ImVec2 previewSize(ImGui::GetContentRegionAvail().x, previewHeight);
            const ImVec2 previewMax(previewPos.x + previewSize.x, previewPos.y + previewSize.y);
            ImDrawList* list = ImGui::GetWindowDrawList();
            list->AddRectFilled(previewPos, previewMax, ImGui::GetColorU32(ImVec4(0.08f, 0.10f, 0.14f, 1.0f)), 3.0f);
            list->AddRect(previewPos, previewMax, ImGui::GetColorU32(ImVec4(0.22f, 0.27f, 0.35f, 1.0f)), 3.0f);

            ImTextureID previewTexId = static_cast<ImTextureID>(0);
            int previewTexWidth = 0;
            int previewTexHeight = 0;
            resolvePreviewTexture(*selectedTemplate, previewTexId, previewTexWidth, previewTexHeight);
            if (previewTexId != static_cast<ImTextureID>(0)) {
                DrawImageCover(list, previewTexId, previewPos, previewMax, previewTexWidth, previewTexHeight, ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 3.0f);
            } else {
                const char* placeholder = selectedTemplate->isBlankPreset ? "Standard 3D Project" : "No Preview Available";
                const ImVec2 textSize = ImGui::CalcTextSize(placeholder);
                list->AddText(ImVec2(previewPos.x + (previewSize.x - textSize.x) * 0.5f,
                                     previewPos.y + (previewSize.y - textSize.y) * 0.5f),
                              ImGui::GetColorU32(ImVec4(0.66f, 0.72f, 0.80f, 1.0f)),
                              placeholder);
            }
            ImGui::Dummy(previewSize);
        }

            const std::string selectedDescription = selectedTemplate->isBlankPreset
                ? (selectedTemplate->description.empty() ? "Creates a clean Modularity project with the standard folders and scripting setup." : selectedTemplate->description)
                : "Copies the selected template project into your new project folder.";
        const std::string sourceLabel = selectedTemplate->isBlankPreset
            ? "Built-in preset"
            : selectedTemplate->projectRoot.filename().string();
        const std::string summaryLabel = std::string(ProjectPipelineLabel(ProjectPipelineFromUiIndex(projectManager.newProjectPipelineMode))) +
            "  •  " +
            Modularity::ToString(projectManager.newProjectRendererMode == 1 ? Modularity::GraphicsBackend::Vulkan
                                                                            : Modularity::GraphicsBackend::OpenGL) +
            "  •  " + sourceLabel;

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextColored(ImVec4(0.94f, 0.97f, 1.0f, 1.0f), "%s", selectedTemplate->displayName.c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(ImVec4(0.60f, 0.67f, 0.76f, 1.0f), "%s", selectedDescription.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(ImVec4(0.72f, 0.78f, 0.86f, 1.0f), "%s", summaryLabel.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextDisabled("Project Name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##ProjectNameDialog", projectManager.newProjectName, sizeof(projectManager.newProjectName));
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextDisabled("Location");
        const float browseWidth = 84.0f;
        ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
        ImGui::InputText("##LocationDialog", projectManager.newProjectLocation, sizeof(projectManager.newProjectLocation));
        ImGui::SameLine();
        if (ImGui::Button("Browse", ImVec2(browseWidth, 0.0f))) {
        }

        if (std::strlen(projectManager.newProjectName) > 0 && std::strlen(projectManager.newProjectLocation) > 0) {
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            fs::path previewPath = fs::path(projectManager.newProjectLocation) / projectManager.newProjectName;
            ImGui::TextColored(ImVec4(0.57f, 0.64f, 0.73f, 1.0f), "Project folder: %s", previewPath.string().c_str());
        }

        if (!projectManager.errorMessage.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s", projectManager.errorMessage.c_str());
        }

        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        const float cancelWidth = 108.0f;
        const float createWidth = 148.0f;
        const float actionsWidth = cancelWidth + createWidth + ImGui::GetStyle().ItemSpacing.x;
        const float actionsOffset = std::max(0.0f, ImGui::GetContentRegionAvail().x - actionsWidth);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + actionsOffset);
        if (ImGui::Button("Cancel", ImVec2(cancelWidth, 0.0f))) {
            projectManager.showNewProjectDialog = false;
            std::memset(projectManager.newProjectName, 0, sizeof(projectManager.newProjectName));
            projectManager.errorMessage.clear();
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.66f, 0.17f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.72f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.84f, 0.60f, 0.13f, 1.0f));
        if (ImGui::Button("Create Project", ImVec2(createWidth, 0.0f))) {
            createProjectFromCurrentState();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndChild();
    }
    ImGui::End();
}
#pragma endregion

#pragma region Open Project Dialog
void Engine::renderOpenProjectDialog() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImVec2 center = mainViewport ? mainViewport->GetCenter()
                                 : ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

    if (mainViewport) {
        ImGui::SetNextWindowViewport(mainViewport->ID);
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 180), ImGuiCond_Appearing);

    if (ImGui::Begin("Open Project", &projectManager.showOpenProjectDialog,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {

        ImGui::Text("Project File Path (.modu):");
        ImGui::SetNextItemWidth(-70);
        ImGui::InputText("##OpenPath", projectManager.openProjectPath,
                       sizeof(projectManager.openProjectPath));
        ImGui::SameLine();
        if (ImGui::Button("Browse")) {
        }

        ImGui::TextDisabled("Select a project.modu file");

        if (!projectManager.errorMessage.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                              projectManager.errorMessage.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float buttonWidth = 100;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttonWidth * 2 - 20);

        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
            projectManager.showOpenProjectDialog = false;
            memset(projectManager.openProjectPath, 0, sizeof(projectManager.openProjectPath));
        }

        ImGui::SameLine();

        if (ImGui::Button("Open", ImVec2(buttonWidth, 0))) {
            if (strlen(projectManager.openProjectPath) == 0) {
                projectManager.errorMessage = "Please enter a project path";
            } else {
                playEditorFeedbackPreview("Resources/Sounds/Selection.mp3", 0.95f, false, EditorFeedbackSoundCategory::Click);
                OpenProjectPath(projectManager.openProjectPath);
                if (!projectManager.errorMessage.empty()) {
                    // Error handled in OpenProjectPath
                } else {
                    projectManager.showOpenProjectDialog = false;
                }
            }
        }
    }
    ImGui::End();
}
#pragma endregion

#pragma region Project Browser Panel
void Engine::renderProjectBrowserPanel() {
    ImVec4 headerCol = ImVec4(0.20f, 0.27f, 0.36f, 1.0f);
    ImVec4 headerColActive = ImVec4(0.24f, 0.34f, 0.46f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Header, headerCol);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, headerColActive);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, headerColActive);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));

    const bool wasOpen = showProjectBrowser;
    ImGui::Begin(Loc::Window("WINDOW_PROJECT_SETTINGS", "Project Settings"), &showProjectBrowser);

    if (!projectManager.currentProject.isLoaded) {
        ImGui::TextDisabled("No project loaded");
        ImGui::End();
        if (wasOpen != showProjectBrowser) {
            saveEditorUserSettings();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        return;
    }

    ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.95f, 1.0f), "%s: %s",
                       Loc::T("SETTINGS_PROJECT_NAME", "Project Name"),
                       projectManager.currentProject.name.c_str());
    if (projectManager.currentProject.hasUnsavedChanges) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "*");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open Modupak Manager")) {
        showRegistryPackagesWindow = true;
        saveEditorUserSettings();
    }

    struct ProjectSettingsUiIcon {
        ImTextureID id = static_cast<ImTextureID>(0);
        bool flipY = false;
    };
    const bool hasVulkanUiImages = usingVulkan() && vulkanRendererInitialized && (vulkanRenderer != nullptr);
    auto resolveProjectSettingsIcon = [&](const char* iconPath) -> ProjectSettingsUiIcon {
        if (!iconPath || !*iconPath) {
            return {};
        }
        if (rendererInitialized) {
            if (Texture* icon = renderer.getTexture(iconPath, MaterialProperties::TextureFilter::Point);
                icon && icon->GetID()) {
                return { static_cast<ImTextureID>(icon->GetID()), true };
            }
        }
        if (hasVulkanUiImages && vulkanRenderer) {
            ImTextureID icon = vulkanRenderer->getOrCreateUIImage(iconPath);
            if (icon != static_cast<ImTextureID>(0)) {
                return { icon, false };
            }
        }
        return {};
    };

    auto drawSettingSectionIcon = [&](const char* iconPath, const char* fallbackIcon,
                                      const char* title, const char* helper,
                                      bool defaultOpen = true) {
        const ProjectSettingsUiIcon icon = resolveProjectSettingsIcon(iconPath);
        return DrawSettingSection(icon.id, icon.flipY, fallbackIcon, title, helper, defaultOpen);
    };

    struct ProjectSettingsTabInfo {
        const char* key;
        const char* label;
        const char* iconPath;
        const char* fallback;
    };

    static constexpr ProjectSettingsTabInfo tabs[] = {
        { "SETTINGS_TAB_SCENES", "Scenes", "Resources/Engine-Root/Project Settings/Tabs/Scenes.png", "S" },
        { "SETTINGS_TAB_ASSETS", "Assets", "Resources/Engine-Root/Project Settings/Tabs/Assets.png", "A" },
        { "SETTINGS_TAB_INPUT_MANAGER", "Input Manager", "Resources/Engine-Root/Project Settings/Tabs/Input Manager.png", "I" },
        { "SETTINGS_TAB_PHYSICS_MANAGER", "Physics Manager", "Resources/Engine-Root/Project Settings/Tabs/Physics Manager.png", "P" },
        { "SETTINGS_TAB_GRAPHICS_MANAGER", "Graphics Manager", "Resources/Engine-Root/Project Settings/Tabs/Graphics.png", "G" },
        { "SETTINGS_TAB_LIGHTING_MANAGER", "Lighting Manager", "Resources/Engine-Root/Project Settings/Tabs/Lighting Manager.png", "L" },
        { "SETTINGS_TAB_TAGS_LAYERS", "Tags & Layers", "Resources/Engine-Root/Project Settings/Tabs/Tags And Layers.png", "T" },
        { "SETTINGS_TAB_CONSOLE_CONFIG", "Console Config", "Resources/Engine-Root/Project Settings/Tabs/Console Config.png", "C" },
        { "SETTINGS_TAB_AUDIO_CONFIG", "Audio Config", "Resources/Engine-Root/Project Settings/Tabs/Audio.png", "A" },
        { "SETTINGS_TAB_PLAYER_CONFIG", "Player Config", "Resources/Engine-Root/Project Settings/Tabs/Player Config.png", "P" },
        { "SETTINGS_TAB_EDITOR", "Editor", "Resources/Engine-Root/Project Settings/Tabs/Editor.png", "E" },
        { "SETTINGS_TAB_BUILD", "Build", "Resources/Engine-Root/Project Settings/Tabs/Build.png", "B" },
        { "SETTINGS_TAB_COMPILATION", "Compilation", "Resources/Engine-Root/Project Settings/Tabs/Compilation.png", "C" },
        { "SETTINGS_TAB_LANGUAGE_MANAGER", "Language Manager", "Resources/Engine-Root/Project Settings/Tabs/Language Manager.png", "L" },
        { "SETTINGS_TAB_OPEN_XR", "OpenXR Manager", "Resources/Engine-Root/Project Settings/Tabs/ModuVR Manager.png", "X" }
    };

    static constexpr int kInputManagerTab = 2;
    static constexpr int kPhysicsManagerTab = 3;
    static constexpr int kGraphicsManagerTab = 4;
    static constexpr int kLightingManagerTab = 5;
    static constexpr int kTagsLayersTab = 6;
    static constexpr int kConsoleConfigTab = 7;
    static constexpr int kAudioConfigTab = 8;
    static constexpr int kPlayerConfigTab = 9;
    static constexpr int kEditorTab = 10;
    static constexpr int kBuildTab = 11;
    static constexpr int kCompilationTab = 12;
    static constexpr int kLanguageManagerTab = 13;
    static constexpr int kOpenXRTab = 14;

    static int selectedTab = 0;
    static bool pendingLanguageFileReload = false;
    if (pendingLanguageFileReload) {
        pendingLanguageFileReload = false;
        Loc::ReloadFromDisk();
        ModuCPPLang::ReloadFromDisk();
        Loc::SetLanguage(projectManager.editorLanguage);
        addConsoleMessage("Reloaded language files from Resources/Languages.", ConsoleMessageType::Success);
    }
    constexpr int tabCount = static_cast<int>(IM_ARRAYSIZE(tabs));
    static std::array<float, tabCount> tabSelectionAnim = {};
    if (selectedTab < 0 || selectedTab >= tabCount) {
        selectedTab = 0;
    }

    const float tabAnimLerpSelected = std::clamp(ImGui::GetIO().DeltaTime * 10.0f, 0.0f, 1.0f);
    const float tabAnimLerpDeselected = std::clamp(ImGui::GetIO().DeltaTime * 16.0f, 0.0f, 1.0f);
    for (int i = 0; i < tabCount; ++i) {
        const float target = (selectedTab == i) ? 1.0f : 0.0f;
        const float lerpT = (selectedTab == i) ? tabAnimLerpSelected : tabAnimLerpDeselected;
        tabSelectionAnim[i] = ImLerp(tabSelectionAnim[i], target, lerpT);
    }

    auto drawProjectSettingsTab = [&](int index, bool compactSidebar) {
        const ProjectSettingsTabInfo& tab = tabs[index];
        const bool isCurrentSelection = (selectedTab == index);
        const float selectedBlend = tabSelectionAnim[index];
        const float pulse = isCurrentSelection ? std::sin(static_cast<float>(ImGui::GetTime()) * 7.5f) : 0.0f;
        const float brighten = std::max(0.0f, pulse) * selectedBlend;
        const float darken = std::max(0.0f, -pulse) * selectedBlend;

        const ImVec2 slotPos = ImGui::GetCursorScreenPos();
        const float compactButtonSize = 44.0f;
        const ImVec2 slotSize = compactSidebar
            ? ImVec2(compactButtonSize, compactButtonSize)
            : ImVec2(ImGui::GetContentRegionAvail().x, 44.0f);
        ImGui::PushID(index);
        const bool pressed = ImGui::InvisibleButton("##ProjectSettingsNavTab", slotSize);
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImGui::PopID();

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImRect slot(slotPos, ImVec2(slotPos.x + slotSize.x, slotPos.y + slotSize.y));
        const float expand = isCurrentSelection
            ? selectedBlend * (0.75f + brighten * 1.35f - darken * 0.55f)
            : selectedBlend * 0.2f;
        const float insetX = compactSidebar ? std::max(1.5f, 2.5f - expand * 0.45f) : std::max(3.0f, 5.0f - expand);
        const float insetY = std::max(1.5f, 2.5f - expand * 0.45f);
        const ImVec2 cardMin(slot.Min.x + insetX, slot.Min.y + insetY);
        const ImVec2 cardMax(slot.Max.x - insetX, slot.Max.y - insetY);
        const float rounding = 13.0f;

        ImVec4 bg = ImVec4(0.12f, 0.14f, 0.19f, 0.92f);
        ImVec4 border = ImVec4(0.22f, 0.27f, 0.36f, 0.90f);
        ImVec4 accent = ImVec4(0.38f, 0.63f, 0.98f, 0.95f);
        if (hovered) {
            bg = ImVec4(0.15f, 0.18f, 0.24f, 0.95f);
            border = ImVec4(0.32f, 0.42f, 0.56f, 0.95f);
        }
        if (selectedBlend > 0.001f) {
            bg = ImVec4(0.14f + brighten * 0.06f - darken * 0.03f,
                        0.20f + brighten * 0.08f - darken * 0.05f,
                        0.30f + brighten * 0.12f - darken * 0.06f,
                        0.96f);
            border = ImVec4(0.32f + brighten * 0.10f,
                            0.58f + brighten * 0.10f,
                            0.95f,
                            0.98f);
            accent = ImVec4(0.45f + brighten * 0.10f,
                            0.78f + brighten * 0.08f,
                            1.0f,
                            0.98f);
        }
        if (held) {
            bg.x += 0.03f;
            bg.y += 0.03f;
            bg.z += 0.03f;
        }

        if (isCurrentSelection && selectedBlend > 0.05f) {
            drawList->AddRectFilled(
                ImVec2(cardMin.x + 1.0f, cardMin.y + 3.0f),
                ImVec2(cardMax.x + 1.0f, cardMax.y + 3.0f),
                ImGui::GetColorU32(ImVec4(0.02f, 0.04f, 0.08f, 0.16f + 0.08f * selectedBlend)),
                rounding);
        }
        drawList->AddRectFilled(cardMin, cardMax, ImGui::GetColorU32(bg), rounding);
        drawList->AddRect(cardMin, cardMax, ImGui::GetColorU32(border), rounding, 0, 1.0f + selectedBlend);
        if (selectedBlend > 0.02f) {
            const float stripeWidth = isCurrentSelection ? 3.0f + brighten * 1.4f : 2.0f;
            if (compactSidebar) {
                drawList->AddRectFilled(
                    ImVec2(cardMin.x + 8.0f, cardMax.y - stripeWidth),
                    ImVec2(cardMax.x - 8.0f, cardMax.y),
                    ImGui::GetColorU32(accent),
                    4.0f);
            } else {
                drawList->AddRectFilled(
                    ImVec2(cardMin.x, cardMin.y + 7.0f),
                    ImVec2(cardMin.x + stripeWidth, cardMax.y - 7.0f),
                    ImGui::GetColorU32(accent),
                    4.0f);
            }
        }

        const ProjectSettingsUiIcon icon = resolveProjectSettingsIcon(tab.iconPath);
        const float cardHeight = cardMax.y - cardMin.y;
        const float baseIconSize = compactSidebar
            ? std::max(24.0f, cardHeight - 11.0f)
            : std::max(24.0f, cardHeight - 9.0f);
        const float iconSize = isCurrentSelection
            ? baseIconSize + selectedBlend * (2.0f + brighten * 1.2f - darken * 0.5f)
            : baseIconSize + selectedBlend * 0.4f;
        const float iconX = compactSidebar
            ? cardMin.x + (cardMax.x - cardMin.x - iconSize) * 0.5f
            : cardMin.x + 10.0f;
        const ImVec2 iconMin(iconX, cardMin.y + (cardMax.y - cardMin.y - iconSize) * 0.5f);
        const ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
        const int iconAlpha = selectedBlend > 0.05f ? 255 : hovered ? 235 : 214;
        if (icon.id != static_cast<ImTextureID>(0)) {
            const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
            const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
            drawList->AddImage(icon.id, iconMin, iconMax, uvMin, uvMax, IM_COL32(255, 255, 255, iconAlpha));
        } else {
            drawList->AddText(
                ImVec2(cardMin.x + 13.0f, cardMin.y + (cardMax.y - cardMin.y - ImGui::GetTextLineHeight()) * 0.5f),
                IM_COL32(255, 255, 255, iconAlpha),
                tab.fallback);
        }

        const char* tabLabel = Loc::T(tab.key, tab.label);
        if (compactSidebar) {
            if (hovered) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(tabLabel);
                ImGui::EndTooltip();
            }
        } else {
            const ImVec2 textSize = ImGui::CalcTextSize(tabLabel);
            const float textX = iconMax.x + 10.0f;
            const float textY = cardMin.y + (cardMax.y - cardMin.y - textSize.y) * 0.5f;
            const ImU32 textColor = ImGui::GetColorU32(
                selectedBlend > 0.05f
                    ? ImVec4(0.95f, 0.98f, 1.0f, 1.0f)
                    : hovered
                        ? ImVec4(0.90f, 0.94f, 0.98f, 1.0f)
                        : ImVec4(0.78f, 0.83f, 0.89f, 1.0f));
            drawList->AddText(ImVec2(textX, textY), textColor, tabLabel);
        }
        return pressed;
    };

    auto selectProjectSettingsTab = [&](int index) {
        if (selectedTab != index) {
            selectedTab = index;
            playEditorFeedbackPreview("Resources/Sounds/Selection Tick Main Editor.mp3", 0.95f, false, EditorFeedbackSoundCategory::Click);
        }
    };
    if (projectSettingsCompactSidebar) {
        ImGui::BeginGroup();
        for (int i = 0; i < tabCount; ++i) {
            if (drawProjectSettingsTab(i, true)) {
                selectProjectSettingsTab(i);
            }
            if (i + 1 < tabCount) {
                const float spacing = (i == 1 || i == kAudioConfigTab || i == kBuildTab) ? 6.0f : 2.0f;
                ImGui::SameLine(0.0f, spacing);
            }
        }
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::BeginChild("SettingsBody", ImVec2(0, 0), false);
    } else {
        ImGui::BeginChild("SettingsNav", ImVec2(214.0f, 0), true);
        if (ImGui::SmallButton("<")) {
            projectSettingsCompactSidebar = true;
            saveEditorUserSettings();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Compact settings categories");
            ImGui::EndTooltip();
        }
        ImGui::Separator();
        for (int i = 0; i < tabCount; ++i) {
            if (drawProjectSettingsTab(i, false)) {
                selectProjectSettingsTab(i);
            }
            if (i + 1 < tabCount) {
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("SettingsBody", ImVec2(0, 0), false);
    }

    static char settingsSearch[160] = "";
    static int settingsModeIndex = 0;
    settingsModeIndex = std::clamp(settingsModeIndex, 0, 2);
    const ProjectSettingsVisibilityMode settingsMode =
        static_cast<ProjectSettingsVisibilityMode>(settingsModeIndex);
    int visibleSettingCount = 0;

    auto visible = [&](ProjectSettingsVisibilityMode required,
                       const char* section,
                       const char* label,
                       const char* tags = nullptr) {
        const bool isVisible = IsSettingVisibleForMode(settingsMode, required) &&
                               ProjectSettingsMatchesSearch(settingsSearch, section, label, tags);
        if (isVisible) {
            ++visibleSettingCount;
        }
        return isVisible;
    };

    auto sectionVisible = [&](const char* section, ProjectSettingsVisibilityMode required) {
        (void)section;
        return IsSettingVisibleForMode(settingsMode, required);
    };

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5.0f, 2.0f));
    const float controlsWidth = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(180.0f, controlsWidth * 0.36f));
    ImGui::InputTextWithHint("##ProjectSettingsSearch", Loc::T("SETTINGS_SEARCH", "Search settings"),
                             settingsSearch, sizeof(settingsSearch));
    ImGui::SameLine();
    const char* modeOptions[] = { Loc::T("SETTINGS_MODE_SIMPLE", "Simple"),
                                  Loc::T("SETTINGS_MODE_ADVANCED", "Advanced"),
                                  Loc::T("SETTINGS_MODE_DEVELOPER", "Developer") };
    ImGui::SetNextItemWidth(148.0f);
    ImGui::Combo("##ProjectSettingsMode", &settingsModeIndex, modeOptions, IM_ARRAYSIZE(modeOptions));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(ProjectSettingsModeHelp(settingsMode));
        ImGui::Separator();
        ImGui::TextUnformatted("Simple: common safe settings.");
        ImGui::TextUnformatted("Advanced: technical user settings.");
        ImGui::TextUnformatted("Developer: debug and internal settings.");
        ImGui::EndTooltip();
    }
    ImGui::Separator();

    static int openXRPlatformTabIndex = 0;
    openXRPlatformTabIndex =
        std::clamp(openXRPlatformTabIndex, 0,
                   static_cast<int>(Modularity::XR::XRPlatformTarget::Count) - 1);
    if (selectedTab == kOpenXRTab) {
        namespace XR = Modularity::XR;
        const int platformCount = static_cast<int>(XR::XRPlatformTarget::Count);
        const ImGuiStyle& style = ImGui::GetStyle();
        const float stripWidth = ImGui::GetContentRegionAvail().x;
        const float tabWidth =
            std::max(64.0f,
                     (stripWidth - style.ItemSpacing.x * static_cast<float>(platformCount - 1)) /
                         static_cast<float>(platformCount));
        const float iconSize = std::min(24.0f, ImGui::GetFontSize() + 8.0f);
        const float tabHeight = iconSize + 14.0f;

        for (int i = 0; i < platformCount; ++i) {
            const auto which = static_cast<XR::XRPlatformTarget>(i);
            const bool isSelected = (openXRPlatformTabIndex == i);
            if (i > 0) ImGui::SameLine();

            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.28f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.23f, 0.32f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.22f, 0.30f, 0.44f, 1.0f));
            if (ImGui::Selectable("##OpenXRPlatformTab", isSelected, ImGuiSelectableFlags_None,
                                  ImVec2(tabWidth, tabHeight))) {
                openXRPlatformTabIndex = i;
            }
            ImGui::PopStyleColor(3);

            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort);
            const ImVec2 rectMin = ImGui::GetItemRectMin();
            const ImVec2 rectMax = ImGui::GetItemRectMax();

            const ProjectSettingsUiIcon icon = resolveProjectSettingsIcon(XR::TabIconPath(which));
            const char* label = XR::DisplayName(which);
            const float labelWidth = ImGui::CalcTextSize(label).x;
            const bool showLabel = (tabWidth >= iconSize + labelWidth + 26.0f);
            const float contentWidth =
                (icon.id ? iconSize : 0.0f) + (showLabel ? labelWidth + 8.0f : 0.0f);
            float cursorX = rectMin.x + ((rectMax.x - rectMin.x) - contentWidth) * 0.5f;
            const float centerY = (rectMin.y + rectMax.y) * 0.5f;

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (icon.id) {
                const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
                const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
                drawList->AddImage(icon.id, ImVec2(cursorX, centerY - iconSize * 0.5f),
                                   ImVec2(cursorX + iconSize, centerY + iconSize * 0.5f), uvMin,
                                   uvMax);
                cursorX += iconSize + 8.0f;
            }
            if (showLabel || !icon.id) {
                const float textX = icon.id ? cursorX
                                            : rectMin.x + ((rectMax.x - rectMin.x) - labelWidth) * 0.5f;
                const ImU32 textColor = ImGui::GetColorU32(
                    isSelected ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.72f, 0.78f, 0.86f, 1.0f));
                drawList->AddText(ImVec2(textX, centerY - ImGui::GetFontSize() * 0.5f), textColor,
                                  label);
            }

            if (projectManager.currentProject.openXRSettings.enabled &&
                projectManager.currentProject.openXRSettings.target(which).enabled) {
                drawList->AddCircleFilled(ImVec2(rectMax.x - 9.0f, rectMin.y + 9.0f), 3.5f,
                                          ImGui::GetColorU32(ImVec4(0.45f, 0.85f, 0.55f, 1.0f)));
            }
            ImGui::PopID();

            if (hovered) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(label);
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                ImGui::TextUnformatted(XR::Description(which));
                ImGui::PopTextWrapPos();
                if (!XR::IsPlatformTargetSupported(which)) {
                    ImGui::TextDisabled("This editor build cannot run a session for this target; "
                                        "the settings still save with the project.");
                }
                ImGui::EndTooltip();
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    static std::array<std::vector<float>, tabCount> settingsSectionHeights;
    BeginProjectSettingsColumns(settingsSectionHeights[selectedTab]);

    if (selectedTab == 0) {
        if (ImGui::Button("+ New Scene")) {
            showNewSceneDialog = true;
            memset(newSceneName, 0, sizeof(newSceneName));
        }

        ImGui::Spacing();

        auto scenes = projectManager.currentProject.getSceneList();
        for (const auto& scene : scenes) {
            if (!ProjectSettingsMatchesSearch(settingsSearch, "Scenes", scene.c_str(), "scene load duplicate delete")) {
                continue;
            }
            ++visibleSettingCount;
            bool isCurrentScene = (scene == projectManager.currentProject.currentSceneName);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                      ImGuiTreeNodeFlags_SpanAvailWidth |
                                      ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (isCurrentScene) flags |= ImGuiTreeNodeFlags_Selected;

            ImGui::TreeNodeEx(scene.c_str(), flags, "[S] %s", scene.c_str());

            if (ImGui::IsItemClicked() && !isCurrentScene) {
                loadScene(scene);
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Load") && !isCurrentScene) {
                    loadScene(scene);
                }
                if (ImGui::MenuItem("Duplicate")) {
                    addConsoleMessage("Scene duplication not yet implemented.", ConsoleMessageType::Info);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete") && !isCurrentScene) {
                    fs::remove(projectManager.currentProject.getSceneFilePath(scene));
                    addConsoleMessage("Deleted scene: " + scene, ConsoleMessageType::Info);
                }
                ImGui::EndPopup();
            }
        }

        if (scenes.empty()) {
            ImGui::TextDisabled("No scenes yet");
        } else if (settingsSearch[0] != '\0' && visibleSettingCount == 0) {
            ImGui::TextDisabled("No scenes match this search.");
        }
    } else if (selectedTab == 1) {
        if (DrawSettingSection("[M]", Loc::T("SETTINGS_ASSETS_LOADED_OBJ_MESHES", "Loaded OBJ Meshes"), Loc::T("SETTINGS_ASSETS_LOADED_OBJ_MESHES_DESC", "Imported .obj meshes available to this project."))) {
            const auto& meshesObj = g_objLoader.getAllMeshes();
            if (meshesObj.empty()) {
                ImGui::TextDisabled("No meshes loaded");
                ImGui::TextDisabled("Import .obj files from File Browser");
            } else {
                for (size_t i = 0; i < meshesObj.size(); i++) {
                    const auto& mesh = meshesObj[i];
                    if (!ProjectSettingsMatchesSearch(settingsSearch, "Assets", mesh.name.c_str(), mesh.path.c_str())) {
                        continue;
                    }
                    ++visibleSettingCount;
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                              ImGuiTreeNodeFlags_SpanAvailWidth |
                                              ImGuiTreeNodeFlags_NoTreePushOnOpen;

                    ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "[M] %s", mesh.name.c_str());

                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("Vertices: %d", mesh.vertexCount);
                        ImGui::Text("Faces: %d", mesh.faceCount);
                        ImGui::Text("Has Normals: %s", mesh.hasNormals ? "Yes" : "No");
                        ImGui::Text("Has UVs: %s", mesh.hasTexCoords ? "Yes" : "No");
                        ImGui::TextDisabled("%s", mesh.path.c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
        }

        if (DrawSettingSection("[A]", Loc::T("SETTINGS_ASSETS_LOADED_MODELS", "Loaded Models"), Loc::T("SETTINGS_ASSETS_LOADED_MODELS_DESC", "Imported Assimp-supported model assets available to this project."))) {
            const auto& meshesAssimp = getModelLoader().getAllMeshes();
            if (meshesAssimp.empty()) {
                ImGui::TextDisabled("No models loaded");
                ImGui::TextDisabled("Import FBX/GLTF/other supported models from File Browser");
            } else {
                for (size_t i = 0; i < meshesAssimp.size(); i++) {
                    const auto& mesh = meshesAssimp[i];
                    if (!ProjectSettingsMatchesSearch(settingsSearch, "Assets", mesh.name.c_str(), mesh.path.c_str())) {
                        continue;
                    }
                    ++visibleSettingCount;
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                              ImGuiTreeNodeFlags_SpanAvailWidth |
                                              ImGuiTreeNodeFlags_NoTreePushOnOpen;

                    ImGui::TreeNodeEx((void*)(intptr_t)(10000 + i), flags, "[A] %s", mesh.name.c_str());

                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("Vertices: %d", mesh.vertexCount);
                        ImGui::Text("Faces: %d", mesh.faceCount);
                        ImGui::Text("Has Normals: %s", mesh.hasNormals ? "Yes" : "No");
                        ImGui::Text("Has UVs: %s", mesh.hasTexCoords ? "Yes" : "No");
                        ImGui::TextDisabled("%s", mesh.path.c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
        }
        if (settingsSearch[0] != '\0' && visibleSettingCount == 0) {
            ImGui::TextDisabled("No assets match this search.");
        }
    } else if (selectedTab == kInputManagerTab) {
        struct InputBindingEntry {
            std::string label;
            std::string keyName;
        };
        struct InputActionEntry {
            std::string name;
            std::string actionType = "Pass Through";
            std::string controlType = "Button";
            bool expanded = false;
            std::vector<InputBindingEntry> bindings;
        };
        struct InputActionMapEntry {
            std::string name;
            std::vector<InputActionEntry> actions;
        };

        static std::vector<InputActionMapEntry> inputActionMaps = []() {
            std::vector<InputActionMapEntry> maps;
            InputActionMapEntry player;
            player.name = "Player";
            player.actions.push_back({ "Jump",    "Pass Through", "Button", true,
                                       { { "Keyboard", "Space" }, { "Gamepad", "Button South" } } });
            player.actions.push_back({ "Sprint",  "Pass Through", "Button", false,
                                       { { "Keyboard", "LeftShift" } } });
            player.actions.push_back({ "Interact","Pass Through", "Button", false,
                                       { { "Keyboard", "E" } } });
            player.actions.push_back({ "Crouch",  "Pass Through", "Button", false,
                                       { { "Keyboard", "C" } } });
            player.actions.push_back({ "Up",      "Pass Through", "Button", false,
                                       { { "Keyboard", "W" }, { "Keyboard", "Up" } } });
            player.actions.push_back({ "Down",    "Pass Through", "Button", false,
                                       { { "Keyboard", "S" }, { "Keyboard", "Down" } } });
            player.actions.push_back({ "Left",    "Pass Through", "Button", false,
                                       { { "Keyboard", "A" }, { "Keyboard", "Left" } } });
            player.actions.push_back({ "Right",   "Pass Through", "Button", false,
                                       { { "Keyboard", "D" }, { "Keyboard", "Right" } } });
            maps.push_back(std::move(player));

            InputActionMapEntry ui;
            ui.name = "UI";
            ui.actions.push_back({ "Submit", "Pass Through", "Button", false,
                                   { { "Keyboard", "Enter" } } });
            ui.actions.push_back({ "Cancel", "Pass Through", "Button", false,
                                   { { "Keyboard", "Escape" } } });
            maps.push_back(std::move(ui));
            return maps;
        }();

        static int selectedMapIndex = 0;
        static int selectedActionIndex = 0;
        if (selectedMapIndex >= (int)inputActionMaps.size()) selectedMapIndex = 0;

        auto writeActionsFile = [&]() {
            if (!projectManager.currentProject.isLoaded) return;
            std::ofstream out(projectManager.currentProject.projectPath / "input_actions.modu");
            if (!out) return;
            out << "# Generated by Input Manager. Format: Action=Key1,Key2,...\n";
            for (const auto& map : inputActionMaps) {
                out << "# Map: " << map.name << "\n";
                for (const auto& action : map.actions) {
                    out << action.name << "=";
                    bool first = true;
                    for (const auto& b : action.bindings) {
                        if (b.keyName.empty()) continue;
                        if (!first) out << ",";
                        out << b.keyName;
                        first = false;
                    }
                    out << "\n";
                }
            }
        };

        if (ImGui::Button("Save Asset")) {
            writeActionsFile();
            addConsoleMessage("Saved input_actions.modu", ConsoleMessageType::Info);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("All Control Schemes  |  All Devices  |  Auto-Save");
        ImGui::Separator();

        const float availY = std::max(260.0f, ImGui::GetContentRegionAvail().y - 6.0f);
        const float colMaps = 150.0f;
        const float colActions = std::max(220.0f, ImGui::GetContentRegionAvail().x * 0.36f);

        // Action Maps
        ImGui::BeginChild("##InputActionMaps", ImVec2(colMaps, availY), true);
        ImGui::TextDisabled("Action Maps");
        ImGui::Separator();
        for (int i = 0; i < (int)inputActionMaps.size(); ++i) {
            const bool selected = (selectedMapIndex == i);
            if (ImGui::Selectable(inputActionMaps[i].name.c_str(), selected)) {
                if (selectedMapIndex != i) {
                    selectedMapIndex = i;
                    selectedActionIndex = 0;
                }
            }
        }
        ImGui::Separator();
        if (ImGui::SmallButton("+ Map")) {
            inputActionMaps.push_back({ "NewMap", {} });
            selectedMapIndex = (int)inputActionMaps.size() - 1;
            selectedActionIndex = 0;
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // Actions
        ImGui::BeginChild("##InputActions", ImVec2(colActions, availY), true);
        ImGui::TextDisabled("Actions");
        ImGui::Separator();
        if (selectedMapIndex >= 0 && selectedMapIndex < (int)inputActionMaps.size()) {
            InputActionMapEntry& map = inputActionMaps[selectedMapIndex];
            if (selectedActionIndex >= (int)map.actions.size()) selectedActionIndex = 0;
            for (int i = 0; i < (int)map.actions.size(); ++i) {
                InputActionEntry& action = map.actions[i];
                ImGui::PushID(i);
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_SpanAvailWidth;
                if (selectedActionIndex == i) flags |= ImGuiTreeNodeFlags_Selected;
                if (action.expanded) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                const bool open = ImGui::TreeNodeEx(action.name.c_str(), flags);
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    selectedActionIndex = i;
                }
                if (open) {
                    action.expanded = true;
                    for (int b = 0; b < (int)action.bindings.size(); ++b) {
                        InputBindingEntry& bind = action.bindings[b];
                        ImGui::PushID(b);
                        ImGui::Bullet();
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s:", bind.label.c_str());
                        ImGui::SameLine();
                        char buf[64] = {};
                        std::snprintf(buf, sizeof(buf), "%s", bind.keyName.c_str());
                        ImGui::SetNextItemWidth(-58.0f);
                        if (ImGui::InputText("##Key", buf, sizeof(buf))) {
                            bind.keyName = buf;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X")) {
                            action.bindings.erase(action.bindings.begin() + b);
                            ImGui::PopID();
                            ImGui::TreePop();
                            ImGui::PopID();
                            goto inputActionsContinue;
                        }
                        ImGui::PopID();
                    }
                    if (ImGui::SmallButton("+ Binding")) {
                        action.bindings.push_back({ "Keyboard", "" });
                    }
                    ImGui::TreePop();
                } else {
                    action.expanded = false;
                }
                ImGui::PopID();
                inputActionsContinue: ;
            }
            ImGui::Separator();
            if (ImGui::SmallButton("+ Action")) {
                map.actions.push_back({ "NewAction", "Pass Through", "Button", true, {} });
                selectedActionIndex = (int)map.actions.size() - 1;
            }
        } else {
            ImGui::TextDisabled("Select an action map.");
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // Properties
        ImGui::BeginChild("##InputActionProperties", ImVec2(0, availY), true);
        ImGui::TextDisabled("Action Properties");
        ImGui::Separator();
        if (selectedMapIndex >= 0 && selectedMapIndex < (int)inputActionMaps.size() &&
            selectedActionIndex >= 0 &&
            selectedActionIndex < (int)inputActionMaps[selectedMapIndex].actions.size()) {
            InputActionEntry& action = inputActionMaps[selectedMapIndex].actions[selectedActionIndex];
            char nameBuf[96] = {};
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", action.name.c_str());
            ImGui::TextDisabled("Name");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##ActionName", nameBuf, sizeof(nameBuf))) {
                action.name = nameBuf;
            }
            ImGui::TextDisabled("Action Type");
            const char* actionTypes[] = { "Pass Through", "Button", "Value" };
            int actionTypeIdx = 0;
            for (int i = 0; i < IM_ARRAYSIZE(actionTypes); ++i) {
                if (action.actionType == actionTypes[i]) { actionTypeIdx = i; break; }
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##ActionType", &actionTypeIdx, actionTypes, IM_ARRAYSIZE(actionTypes))) {
                action.actionType = actionTypes[actionTypeIdx];
            }
            ImGui::TextDisabled("Control Type");
            const char* controlTypes[] = { "Button", "Axis", "Vector2", "Vector3" };
            int controlTypeIdx = 0;
            for (int i = 0; i < IM_ARRAYSIZE(controlTypes); ++i) {
                if (action.controlType == controlTypes[i]) { controlTypeIdx = i; break; }
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##ControlType", &controlTypeIdx, controlTypes, IM_ARRAYSIZE(controlTypes))) {
                action.controlType = controlTypes[controlTypeIdx];
            }
            ImGui::Separator();
            ImGui::TextDisabled("Interactions");
            ImGui::TextDisabled("(No interactions configured)");
            ImGui::Separator();
            ImGui::TextDisabled("Processors");
            ImGui::TextDisabled("(No processors configured)");
            ImGui::Separator();
            ImGui::TextDisabled("Scripts use: Input.ButtonDown(\"%s\"), .ButtonHeld, .ButtonUp", action.name.c_str());
        } else {
            ImGui::TextDisabled("Select an action to edit its properties.");
        }
        ImGui::EndChild();
    } else if (selectedTab == kPhysicsManagerTab) {
        ProjectPhysicsSettings& physicsSettings = projectManager.currentProject.physicsSettings;
        bool changed = false;

        if (DrawSettingSection("[G]", Loc::T("SETTINGS_PHYSICS_GLOBAL_SIMULATION", "Global Simulation"), Loc::T("SETTINGS_PHYSICS_GLOBAL_SIMULATION_DESC", "Project-wide gravity, timing, and solver defaults."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Global Simulation", "Physics Backend", "physics engine backend jolt physx")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_PHYSICS_BACKEND", "Physics Backend"),
                                          Loc::T("SETTINGS_PHYSICS_PHYSICS_BACKEND_DESC", "Engine used for 3D simulation. Jolt is the cross-platform default (works on Android). PhysX is available on desktop only. Switching rebuilds the active physics world."),
                                          [&]() {
                    const PhysicsBackendType options[] = { PhysicsBackendType::Jolt, PhysicsBackendType::PhysX };
                    int currentIdx = (physicsSettings.backend == PhysicsBackendType::PhysX) ? 1 : 0;
                    const char* preview = PhysicsBackendLabel(physicsSettings.backend);
                    bool rowChanged = false;
                    if (ImGui::BeginCombo("##PhysicsBackend", preview)) {
                        for (int i = 0; i < (int)(sizeof(options) / sizeof(options[0])); ++i) {
                            const bool selected = (i == currentIdx);
                            if (ImGui::Selectable(PhysicsBackendLabel(options[i]), selected)) {
                                if (options[i] != physicsSettings.backend) {
                                    physicsSettings.backend = options[i];
                                    if (physics) physics->shutdown();
                                    physics = CreatePhysicsBackend(physicsSettings.backend);
                                    if (physics) {
                                        physics->setProjectSettings(physicsSettings);
                                        physics->init();
                                    }
                                    rowChanged = true;
                                }
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    return rowChanged;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Global Simulation", "Global Gravity", "gravity physics")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_GLOBAL_GRAVITY", "Global Gravity"), Loc::T("SETTINGS_PHYSICS_GLOBAL_GRAVITY_DESC", "3D and 2D runtime gravity use this project-wide multiplier."), [&]() {
                    if (ImGui::DragFloat("##PhysicsGlobalGravity", &physicsSettings.globalGravityScale, 0.01f, 0.0f, 10.0f, "%.2f")) {
                        physicsSettings.globalGravityScale = std::clamp(physicsSettings.globalGravityScale, 0.0f, 10.0f);
                        physics->setProjectSettings(physicsSettings);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Global Simulation", "Physics Timestep", "fixed timestep")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_PHYSICS_TIMESTEP", "Physics Timestep"), Loc::T("SETTINGS_PHYSICS_PHYSICS_TIMESTEP_DESC", "Fixed simulation step used by project physics."), [&]() {
                    if (ImGui::DragFloat("##PhysicsTimestep", &physicsSettings.fixedTimestep, 0.001f, 0.001f, 0.1f, "%.4f s")) {
                        physicsSettings.fixedTimestep = std::clamp(physicsSettings.fixedTimestep, 0.001f, 0.1f);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Global Simulation", "Solver Iterations", "stability constraint solver")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_SOLVER_ITERATIONS", "Solver Iterations"), Loc::T("SETTINGS_PHYSICS_SOLVER_ITERATIONS_DESC", "More iterations improve stability at a CPU cost."), [&]() {
                    if (ImGui::DragInt("##PhysicsSolverIterations", &physicsSettings.solverIterations, 1.0f, 1, 64)) {
                        physicsSettings.solverIterations = std::clamp(physicsSettings.solverIterations, 1, 64);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Global Simulation", "Physics Modes", "2d 3d toggles")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_PHYSICS_MODES", "Physics Modes"), Loc::T("SETTINGS_PHYSICS_PHYSICS_MODES_DESC", "Enable the physics systems this project expects to use."), [&]() {
                    bool rowChanged = ImGui::Checkbox("3D##Physics3D", &physicsSettings.enable3DPhysics);
                    ImGui::SameLine();
                    rowChanged |= ImGui::Checkbox("2D##Physics2D", &physicsSettings.enable2DPhysics);
                    return rowChanged;
                });
            }
        }

        if (DrawSettingSection("[R]", Loc::T("SETTINGS_PHYSICS_RAYCASTS", "Raycasts"), Loc::T("SETTINGS_PHYSICS_RAYCASTS_DESC", "Default raycast behavior for project tools and runtime helpers."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Raycasts", "Default Distance", "raycast distance")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_DEFAULT_DISTANCE", "Default Distance"), Loc::T("SETTINGS_PHYSICS_DEFAULT_DISTANCE_DESC", "Fallback max distance for raycast-style project defaults."), [&]() {
                    if (ImGui::DragFloat("##DefaultRaycastDistance", &physicsSettings.defaultRaycastDistance, 1.0f, 0.01f, 100000.0f, "%.1f")) {
                        physicsSettings.defaultRaycastDistance = std::max(0.01f, physicsSettings.defaultRaycastDistance);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Raycasts", "Hit Triggers", "trigger colliders")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_HIT_TRIGGERS", "Hit Triggers"), Loc::T("SETTINGS_PHYSICS_HIT_TRIGGERS_DESC", "Whether default raycasts should include trigger-style colliders."), [&]() {
                    return ImGui::Checkbox("##RaycastHitTriggers", &physicsSettings.raycastHitTriggers);
                });
            }
        }

        if (DrawSettingSection("[B]", Loc::T("SETTINGS_PHYSICS_RIGIDBODY_DEFAULTS", "Rigidbody Defaults"), Loc::T("SETTINGS_PHYSICS_RIGIDBODY_DEFAULTS_DESC", "Default values used when new rigidbodies are added."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Rigidbody Defaults", "Default Mass", "mass rigidbody")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_DEFAULT_MASS", "Default Mass"), Loc::T("SETTINGS_PHYSICS_DEFAULT_MASS_DESC", "New Rigidbody mass before per-object edits."), [&]() {
                    if (ImGui::DragFloat("##DefaultRigidbodyMass", &physicsSettings.defaultRigidbodyMass, 0.05f, 0.0001f, 10000.0f, "%.3f")) {
                        physicsSettings.defaultRigidbodyMass = std::max(0.0001f, physicsSettings.defaultRigidbodyMass);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Rigidbody Defaults", "Default Drag", "drag damping")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_DEFAULT_DRAG", "Default Drag"), Loc::T("SETTINGS_PHYSICS_DEFAULT_DRAG_DESC", "New Rigidbody linear drag before per-object edits."), [&]() {
                    if (ImGui::DragFloat("##DefaultRigidbodyDrag", &physicsSettings.defaultRigidbodyDrag, 0.01f, 0.0f, 100.0f, "%.3f")) {
                        physicsSettings.defaultRigidbodyDrag = std::max(0.0f, physicsSettings.defaultRigidbodyDrag);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Rigidbody Defaults", "Mass Units", "kilograms grams pounds ounces")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_MASS_UNITS", "Mass Units"), Loc::T("SETTINGS_PHYSICS_MASS_UNITS_DESC", "Changes how Rigidbody mass values are shown and converted."), [&]() {
                    int massUnitIndex = static_cast<int>(physicsSettings.massUnit);
                    const char* massUnitOptions[] = { "Kilograms (kg)", "Grams (g)", "Pounds (lb)", "Ounces (oz)" };
                    if (ImGui::Combo("##PhysicsMassUnitsManager", &massUnitIndex, massUnitOptions, IM_ARRAYSIZE(massUnitOptions))) {
                        physicsSettings.massUnit = static_cast<ProjectMassUnit>(
                            std::clamp(massUnitIndex, 0, static_cast<int>(IM_ARRAYSIZE(massUnitOptions)) - 1));
                        return true;
                    }
                    return false;
                });
            }
        }

        if (DrawSettingSection("[M]", Loc::T("SETTINGS_PHYSICS_DEFAULT_PHYSICS_MATERIAL", "Default Physics Material"), Loc::T("SETTINGS_PHYSICS_DEFAULT_PHYSICS_MATERIAL_DESC", "Surface behavior used by newly authored physics materials."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Default Physics Material", "Friction", "material friction")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_FRICTION", "Friction"), Loc::T("SETTINGS_PHYSICS_FRICTION_DESC", "Default surface friction."), [&]() {
                    if (ImGui::SliderFloat("##DefaultMaterialFriction", &physicsSettings.defaultMaterialFriction, 0.0f, 1.0f, "%.2f")) {
                        physicsSettings.defaultMaterialFriction = std::clamp(physicsSettings.defaultMaterialFriction, 0.0f, 1.0f);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Default Physics Material", "Bounciness", "restitution")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_PHYSICS_BOUNCINESS", "Bounciness"), Loc::T("SETTINGS_PHYSICS_BOUNCINESS_DESC", "Default restitution for new physics materials."), [&]() {
                    if (ImGui::SliderFloat("##DefaultMaterialBounciness", &physicsSettings.defaultMaterialBounciness, 0.0f, 1.0f, "%.2f")) {
                        physicsSettings.defaultMaterialBounciness = std::clamp(physicsSettings.defaultMaterialBounciness, 0.0f, 1.0f);
                        return true;
                    }
                    return false;
                });
            }
        }

        if (changed) {
            projectManager.currentProject.saveProjectFile();
            projectManager.currentProject.hasUnsavedChanges = true;
        }
    } else if (selectedTab == kGraphicsManagerTab) {
        ProjectGraphicsSettings& graphics = projectManager.currentProject.graphicsSettings;
        bool changed = false;
        bool buildSettingsChanged = false;

        if (DrawSettingSection("[B]", Loc::T("SETTINGS_GRAPHICS_RENDERER_BACKEND", "Renderer Backend"), Loc::T("SETTINGS_GRAPHICS_RENDERER_BACKEND_DESC", "Renderer API and OpenGL renderer controls."))) {
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Renderer Backend", "Renderer API", "opengl vulkan backend")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_RENDERER_API", "Renderer API"), Loc::T("SETTINGS_GRAPHICS_RENDERER_API_DESC", "Changing this may require an editor restart."), [&]() {
                    int rendererIndex = (projectManager.currentProject.rendererBackend == Modularity::GraphicsBackend::Vulkan) ? 1 : 0;
                    const char* rendererOptions[] = { "OpenGL", "Vulkan (Experimental)" };
                    if (ImGui::Combo("##ProjectRendererApiGraphics", &rendererIndex, rendererOptions, IM_ARRAYSIZE(rendererOptions))) {
#if !MODULARITY_HAS_VULKAN
                        if (rendererIndex == 1) {
                            rendererIndex = 0;
                            addConsoleMessage("Vulkan renderer is unavailable in this build. Keeping OpenGL.",
                                              ConsoleMessageType::Warning);
                        }
#else
                        if (rendererIndex == 1 && !hasVulkanPipelinePackage()) {
                            rendererIndex = 0;
                            addConsoleMessage("Install moduengine.vulkan-pipeline to enable Vulkan. Keeping OpenGL.",
                                              ConsoleMessageType::Warning);
                        }
#endif
                        projectManager.currentProject.rendererBackend =
                            (rendererIndex == 1) ? Modularity::GraphicsBackend::Vulkan : Modularity::GraphicsBackend::OpenGL;
                        return true;
                    }
                    return false;
                });
                if (projectManager.currentProject.rendererBackend != graphicsBackend) {
                    DrawHelpText("Current session uses a different renderer. Restart editor to apply this project renderer.");
                }
            }

            const bool openGlSettingsAvailable = rendererInitialized && !usingVulkan();
            if (!openGlSettingsAvailable && IsSettingVisibleForMode(settingsMode, ProjectSettingsVisibilityMode::Advanced)) {
                DrawHelpText("OpenGL renderer settings are editable only in OpenGL sessions.");
            }
            ImGui::BeginDisabled(!openGlSettingsAvailable);
            if (visible(ProjectSettingsVisibilityMode::Simple, "Renderer Backend", "Ambient Color", "light color")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_AMBIENT_COLOR", "Ambient Color"), Loc::T("SETTINGS_GRAPHICS_AMBIENT_COLOR_DESC", "Base light color used by the OpenGL renderer."), [&]() {
                    glm::vec3 ambient = renderer.getAmbientColor();
                    if (ImGui::ColorEdit3("##RendererAmbientColorGraphics", &ambient.x)) {
                        renderer.setAmbientColor(ambient);
                        buildSettings.rendererAmbientColor = ambient;
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Renderer Backend", "Shadow Resolution", "shadow map quality")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_SHADOW_RESOLUTION", "Shadow Resolution"), Loc::T("SETTINGS_GRAPHICS_SHADOW_RESOLUTION_DESC", "Higher values can look sharper but cost more GPU memory."), [&]() {
                    int shadowResolution = renderer.getShadowMapResolution();
                    if (ImGui::SliderInt("##RendererShadowResolutionGraphics", &shadowResolution, 128, 4096)) {
                        renderer.setShadowMapResolution(shadowResolution);
                        buildSettings.rendererShadowResolution = renderer.getShadowMapResolution();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Renderer Backend", "Auto Reload Shaders", "developer shader hot reload")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_AUTO_RELOAD_SHADERS", "Auto Reload Shaders"), Loc::T("SETTINGS_GRAPHICS_AUTO_RELOAD_SHADERS_DESC", "Reloads shaders while editing. Useful for shader development."), [&]() {
                    bool shaderAutoReload = renderer.isShaderAutoReloadEnabled();
                    if (ImGui::Checkbox("##RendererAutoReloadShadersGraphics", &shaderAutoReload)) {
                        renderer.setShaderAutoReload(shaderAutoReload);
                        buildSettings.rendererAutoReloadShaders = shaderAutoReload;
                        return true;
                    }
                    return false;
                });
            }
            ImGui::EndDisabled();
        }

        if (DrawSettingSection("[R]", Loc::T("SETTINGS_GRAPHICS_RUNTIME_GRAPHICS", "Runtime Graphics"), Loc::T("SETTINGS_GRAPHICS_RUNTIME_GRAPHICS_DESC", "Startup graphics behavior for the player."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Runtime Graphics", "VSync", "sync")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_VSYNC", "VSync"), Loc::T("SETTINGS_GRAPHICS_VSYNC_DESC", "Synchronize presentation to the display refresh."), [&]() {
                    if (ImGui::Checkbox("##GraphicsVSync", &graphics.vsync)) {
                        applyPresentationSettings();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Runtime Graphics", "Target FPS", "frame cap")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_TARGET_FPS", "Target FPS"), Loc::T("SETTINGS_GRAPHICS_TARGET_FPS_DESC", "Runtime frame-rate target when VSync is not controlling presentation."), [&]() {
                    if (ImGui::DragInt("##GraphicsTargetFps", &graphics.targetFps, 1.0f, 1, 500)) {
                        applyPresentationSettings();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Runtime Graphics", "Startup Mode", "fullscreen windowed")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_STARTUP_MODE", "Startup Mode"), Loc::T("SETTINGS_GRAPHICS_STARTUP_MODE_DESC", "Default player window mode."), [&]() {
                    int mode = graphics.fullscreenStartup ? 1 : 0;
                    const char* modes[] = { "Windowed", "Fullscreen" };
                    if (ImGui::Combo("##GraphicsStartupMode", &mode, modes, IM_ARRAYSIZE(modes))) {
                        graphics.fullscreenStartup = (mode == 1);
                        projectManager.currentProject.playerSettings.fullscreenStartup = graphics.fullscreenStartup;
                        return true;
                    }
                    return false;
                });
            }
        }

        if (DrawSettingSection("[Q]", Loc::T("SETTINGS_GRAPHICS_QUALITY", "Quality"), Loc::T("SETTINGS_GRAPHICS_QUALITY_DESC", "Renderer quality defaults."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Quality", "Shadow Quality", "shadow resolution")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_SHADOW_QUALITY", "Shadow Quality"), Loc::T("SETTINGS_GRAPHICS_SHADOW_QUALITY_DESC", "Preset for shadow-map quality."), [&]() {
                    const char* quality[] = { "Off", "Low", "Medium", "High" };
                    return ImGui::Combo("##ShadowQuality", &graphics.shadowQuality, quality, IM_ARRAYSIZE(quality));
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Quality", "Render Scale", "resolution scale global render scale")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_RENDER_SCALE", "Render Scale"), Loc::T("SETTINGS_GRAPHICS_RENDER_SCALE_DESC", "Scales the runtime render resolution globally. Below 1 renders fewer pixels and stretches up; above 1 supersamples."), [&]() {
                    return ImGui::SliderFloat("##RenderResolutionScale", &graphics.renderResolutionScale, 0.25f, 2.0f, "%.2fx");
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Quality", "HDR", "high dynamic range bloom")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_HDR", "HDR"), Loc::T("SETTINGS_GRAPHICS_HDR_DESC", "Renders to float color buffers so bright values survive for bloom and tone mapping. Turning it off forces 8-bit color."), [&]() {
                    if (ImGui::Checkbox("##GraphicsHdr", &graphics.hdr)) {
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Quality", "Color Resolution", "color precision 8-bit 16-bit float buffer format")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_COLOR_RESOLUTION", "Color Resolution"), Loc::T("SETTINGS_GRAPHICS_COLOR_RESOLUTION_DESC", "Storage precision of the offscreen color buffers. Auto keeps 16-bit float; 8-bit halves bandwidth for strictly LDR projects."), [&]() {
                    int colorRes = static_cast<int>(graphics.colorResolution);
                    const char* colorResOptions[] = { "Auto", "8-bit", "16-bit Float" };
                    if (ImGui::Combo("##GraphicsColorResolution", &colorRes, colorResOptions, IM_ARRAYSIZE(colorResOptions))) {
                        graphics.colorResolution = static_cast<ProjectColorResolution>(std::clamp(colorRes, 0, 2));
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
                if (!graphics.hdr) {
                    DrawHelpText("HDR is off, so color buffers stay 8-bit regardless of this pick.");
                }
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Quality", "Texture Filtering", "point bilinear trilinear")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_TEXTURE_FILTERING", "Texture Filtering"), Loc::T("SETTINGS_GRAPHICS_TEXTURE_FILTERING_DESC", "Default sampling mode for newly authored materials."), [&]() {
                    int filter = static_cast<int>(graphics.textureFiltering);
                    const char* filters[] = { "Bilinear", "Point", "Trilinear" };
                    if (ImGui::Combo("##TextureFiltering", &filter, filters, IM_ARRAYSIZE(filters))) {
                        graphics.textureFiltering = static_cast<ProjectTextureFiltering>(std::clamp(filter, 0, 2));
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Quality", "Global Texture Format", "texture format auto compression vram 16bpp")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_GLOBAL_TEXTURE_FORMAT", "Global Texture Format"), Loc::T("SETTINGS_GRAPHICS_GLOBAL_TEXTURE_FORMAT_DESC", "GPU storage format every texture defaults to. Auto adapts per texture; per-texture overrides in the File Browser still win."), [&]() {
                    const TextureFormatPolicy formatOptions[] = {
                        TextureFormatPolicy::Auto, TextureFormatPolicy::Full,
                        TextureFormatPolicy::RGB565, TextureFormatPolicy::RGB5_A1,
                        TextureFormatPolicy::RGBA4
                    };
                    const char* formatLabels[] = {
                        "Auto (adaptive)", "Full (RGBA8)", "RGB565 (16bpp opaque)",
                        "RGB5_A1 (16bpp cutout)", "RGBA4 (16bpp blended)"
                    };
                    const TextureFormatPolicy current = TextureFormatPolicyFromString(graphics.defaultTextureFormat);
                    int formatIdx = 0;
                    for (int i = 0; i < (int)(sizeof(formatOptions) / sizeof(formatOptions[0])); ++i) {
                        if (formatOptions[i] == current) { formatIdx = i; break; }
                    }
                    if (ImGui::Combo("##GlobalTextureFormat", &formatIdx, formatLabels, IM_ARRAYSIZE(formatLabels))) {
                        graphics.defaultTextureFormat = ToString(formatOptions[std::clamp(formatIdx, 0, 4)]);
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Quality", "Max Texture Size", "texture size memory vram")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_GRAPHICS_MAX_TEXTURE_SIZE", "Max Texture Size"),
                    Loc::T("SETTINGS_GRAPHICS_MAX_TEXTURE_SIZE_DESC",
                           "Largest dimension textures are uploaded at. Bigger images are "
                           "downscaled on load. A 4K texture costs 85 MB of VRAM with mips, "
                           "so a few 4K material sets will oversubscribe VRAM and stutter as "
                           "the driver pages them; 2048 costs a quarter of that."),
                    [&]() {
                        const int sizeOptions[] = { 0, 512, 1024, 2048, 4096, 8192 };
                        const char* sizeLabels[] = {
                            "No limit (full size)", "512", "1024", "2048 (recommended)",
                            "4096", "8192"
                        };
                        int sizeIdx = 3;
                        for (int i = 0; i < (int)(sizeof(sizeOptions) / sizeof(sizeOptions[0])); ++i) {
                            if (sizeOptions[i] == graphics.maxTextureSize) { sizeIdx = i; break; }
                        }
                        if (ImGui::Combo("##MaxTextureSize", &sizeIdx, sizeLabels, IM_ARRAYSIZE(sizeLabels))) {
                            graphics.maxTextureSize = sizeOptions[std::clamp(sizeIdx, 0, 5)];
                            applyProjectGraphicsToRenderer();
                            return true;
                        }
                        return false;
                    });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Quality", "Shadow Filtering", "shadow soft pcf")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_GRAPHICS_SHADOW_FILTER", "Shadow Filtering"),
                    Loc::T("SETTINGS_GRAPHICS_SHADOW_FILTER_DESC",
                           "How many samples soften a shadow edge. The old filter took nine "
                           "samples however wide the penumbra, which showed as concentric "
                           "bands rather than a gradient."),
                    [&]() {
                        const char* qualityLabels[] = {
                            "Hard (no filtering)", "Low (8 taps)",
                            "Medium (16 taps)", "High (32 taps)"
                        };
                        int q = std::clamp(graphics.shadowFilterQuality, 0, 3);
                        if (ImGui::Combo("##ShadowFilterQuality", &q, qualityLabels,
                                         IM_ARRAYSIZE(qualityLabels))) {
                            graphics.shadowFilterQuality = q;
                            applyProjectGraphicsToRenderer();
                            return true;
                        }
                        return false;
                    });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Quality", "Shadow Detail Distance", "shadow lod")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_GRAPHICS_SHADOW_LOD", "Shadow Detail Distance"),
                    Loc::T("SETTINGS_GRAPHICS_SHADOW_LOD_DESC",
                           "Past this distance the shadow tap count halves, and halves again "
                           "at twice it. 0 pays full price everywhere."),
                    [&]() {
                        float d = graphics.shadowLodDistance;
                        if (ImGui::SliderFloat("##ShadowLodDistance", &d, 0.0f, 200.0f, "%.0f m")) {
                            graphics.shadowLodDistance = std::max(0.0f, d);
                            applyProjectGraphicsToRenderer();
                            return true;
                        }
                        return false;
                    });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Quality", "Contact Hardening", "shadow pcss")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_GRAPHICS_SHADOW_PCSS", "Contact Hardening"),
                    Loc::T("SETTINGS_GRAPHICS_SHADOW_PCSS_DESC",
                           "Size each penumbra from the gap between occluder and receiver, so "
                           "a shadow is tight where it touches and widens with distance."),
                    [&]() {
                        bool v = graphics.shadowContactHardening;
                        if (ImGui::Checkbox("##ShadowContactHardening", &v)) {
                            graphics.shadowContactHardening = v;
                            applyProjectGraphicsToRenderer();
                            return true;
                        }
                        return false;
                    });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Quality", "Anti-aliasing", "msaa")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_ANTI_ALIASING", "Anti-aliasing"), Loc::T("SETTINGS_GRAPHICS_ANTI_ALIASING_DESC", "Default anti-aliasing level for player rendering."), [&]() {
                    int aa = static_cast<int>(graphics.antiAliasing);
                    const char* aaOptions[] = { "Off", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
                    if (ImGui::Combo("##AntiAliasing", &aa, aaOptions, IM_ARRAYSIZE(aaOptions))) {
                        graphics.antiAliasing = static_cast<ProjectAntiAliasing>(std::clamp(aa, 0, 3));
                        return true;
                    }
                    return false;
                });
            }
        }

        if (DrawSettingSection("[P]", Loc::T("SETTINGS_GRAPHICS_PREVIEW_OVERRIDES", "Preview Overrides"), Loc::T("SETTINGS_GRAPHICS_PREVIEW_OVERRIDES_DESC", "Editor and game preview graphics overrides."))) {
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Preview Overrides", "Editor Preview Overrides", "editor preview")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_EDITOR_PREVIEW_OVERRIDES", "Editor Preview Overrides"), Loc::T("SETTINGS_GRAPHICS_EDITOR_PREVIEW_OVERRIDES_DESC", "Allow editor previews to use editor-specific graphics settings."), [&]() {
                    return ImGui::Checkbox("##EditorPreviewGraphicsOverrides", &graphics.editorPreviewOverrides);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Preview Overrides", "Game Preview Overrides", "game preview")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_GRAPHICS_GAME_PREVIEW_OVERRIDES", "Game Preview Overrides"), Loc::T("SETTINGS_GRAPHICS_GAME_PREVIEW_OVERRIDES_DESC", "Allow the Game View to override runtime graphics defaults."), [&]() {
                    return ImGui::Checkbox("##GamePreviewGraphicsOverrides", &graphics.gamePreviewOverrides);
                });
            }
        }

        if (changed) {
            graphics.targetFps = std::clamp(graphics.targetFps, 1, 500);
            graphics.shadowQuality = std::clamp(graphics.shadowQuality, 0, 3);
            graphics.renderResolutionScale = std::clamp(graphics.renderResolutionScale, 0.25f, 2.0f);
            projectManager.currentProject.saveProjectFile();
        }
        if (buildSettingsChanged) {
            saveBuildSettings();
        }
    } else if (selectedTab == kLightingManagerTab) {
        ProjectGraphicsSettings& graphics = projectManager.currentProject.graphicsSettings;
        bool changed = false;
        bool buildSettingsChanged = false;

        int activeDirectional = 0;
        int activeOther = 0;
        for (const SceneObject& lightObj : sceneObjects) {
            if (!IsObjectEnabledInHierarchy(lightObj) || !lightObj.hasLight || !lightObj.light.enabled) continue;
            if (lightObj.light.type == LightType::Directional) ++activeDirectional;
            else ++activeOther;
        }
        int pathLightBudget = 20;
        int pathDirectionalAllowance = 1;
        ProjectRenderingPathCaps(graphics.renderingPath, pathLightBudget, pathDirectionalAllowance);

        if (DrawSettingSection("[P]", Loc::T("SETTINGS_LIGHTING_RENDERING_PATH_2", "Rendering Path"), Loc::T("SETTINGS_LIGHTING_RENDERING_PATH_2_DESC", "Light budget mode, Modularity's take on Forward/Deferred."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Rendering Path", "Rendering Path", "normal deferred forward light limit budget")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_RENDERING_PATH", "Rendering Path"), Loc::T("SETTINGS_LIGHTING_RENDERING_PATH_DESC", "How many realtime lights a scene may use. Directional lights keep guaranteed slots on top of the budget."), [&]() {
                    const ProjectRenderingPath pathOptions[] = {
                        ProjectRenderingPath::Normal, ProjectRenderingPath::NormalPlus,
                        ProjectRenderingPath::Deferred, ProjectRenderingPath::HeavyDeferred
                    };
                    bool rowChanged = false;
                    if (ImGui::BeginCombo("##RenderingPath", ProjectRenderingPathLabel(graphics.renderingPath))) {
                        for (ProjectRenderingPath option : pathOptions) {
                            const bool selected = (option == graphics.renderingPath);
                            if (ImGui::Selectable(ProjectRenderingPathLabel(option), selected)) {
                                if (option != graphics.renderingPath) {
                                    graphics.renderingPath = option;
                                    applyProjectGraphicsToRenderer();
                                    rowChanged = true;
                                }
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", ProjectRenderingPathNote(option));
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    return rowChanged;
                });
                DrawHelpText(ProjectRenderingPathNote(graphics.renderingPath));
                const bool overBudget = activeOther > pathLightBudget ||
                                        activeDirectional > pathDirectionalAllowance;
                ImGui::TextColored(overBudget ? ImVec4(1.0f, 0.55f, 0.35f, 1.0f)
                                              : ImVec4(0.55f, 0.75f, 0.55f, 1.0f),
                                   "Scene: %d light%s + %d directional (budget %d + %d directional)",
                                   activeOther, activeOther == 1 ? "" : "s", activeDirectional,
                                   pathLightBudget, pathDirectionalAllowance);
                if (overBudget) {
                    DrawHelpText("Over budget: the renderer keeps the nearest lights and drops the rest each frame.");
                }
            }
        }

        const bool lightingEditable = rendererInitialized && !usingVulkan();
        if (!lightingEditable) {
            DrawHelpText("Lighting settings apply to the OpenGL renderer and are editable only in OpenGL sessions.");
        }
        ImGui::BeginDisabled(!lightingEditable);

        if (DrawSettingSection("[M]", Loc::T("SETTINGS_LIGHTING_MAIN_LIGHT_2", "Main Light"), Loc::T("SETTINGS_LIGHTING_MAIN_LIGHT_2_DESC", "The directional light(s) with guaranteed slots."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Main Light", "Main Light", "directional sun per pixel off")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_MAIN_LIGHT", "Main Light"), Loc::T("SETTINGS_LIGHTING_MAIN_LIGHT_DESC", "Per Pixel keeps directional lighting on; Off removes directional lights from shading."), [&]() {
                    int mode = graphics.mainLightEnabled ? 0 : 1;
                    const char* modes[] = { "Per Pixel", "Off" };
                    if (ImGui::Combo("##MainLightMode", &mode, modes, IM_ARRAYSIZE(modes))) {
                        graphics.mainLightEnabled = (mode == 0);
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Main Light", "Cast Shadows", "directional shadows")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_CAST_SHADOWS", "Cast Shadows"), Loc::T("SETTINGS_LIGHTING_CAST_SHADOWS_DESC", "Whether directional lights may render shadow maps."), [&]() {
                    if (ImGui::Checkbox("##MainLightCastShadows", &graphics.mainLightCastShadows)) {
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
            }
        }

        if (DrawSettingSection("[A]", Loc::T("SETTINGS_LIGHTING_ADDITIONAL_LIGHTS_2", "Additional Lights"), Loc::T("SETTINGS_LIGHTING_ADDITIONAL_LIGHTS_2_DESC", "Point, spot, and area lights competing for the budget."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Additional Lights", "Additional Lights", "point spot area per pixel off")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_ADDITIONAL_LIGHTS", "Additional Lights"), Loc::T("SETTINGS_LIGHTING_ADDITIONAL_LIGHTS_DESC", "Per Pixel keeps point/spot/area lights on; Off drops them from shading."), [&]() {
                    int mode = graphics.additionalLightsEnabled ? 0 : 1;
                    const char* modes[] = { "Per Pixel", "Off" };
                    if (ImGui::Combo("##AdditionalLightsMode", &mode, modes, IM_ARRAYSIZE(modes))) {
                        graphics.additionalLightsEnabled = (mode == 0);
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Additional Lights", "Per Frame Limit", "max realtime lights per frame")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_PER_FRAME_LIMIT", "Per Frame Limit"), Loc::T("SETTINGS_LIGHTING_PER_FRAME_LIMIT_DESC", "Hard cap on lights uploaded to the shader each frame (nearest lights win). The rendering path budget still applies on top."), [&]() {
                    int maxLights = renderer.getMaxRealtimeLights();
                    if (ImGui::SliderInt("##LightingMaxRealtimeLights", &maxLights, 1, kRendererMaxRealtimeLights)) {
                        renderer.setMaxRealtimeLights(maxLights);
                        saveEditorUserSettings();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Additional Lights", "Cast Shadows", "point spot shadows")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_CAST_SHADOWS_2", "Cast Shadows"), Loc::T("SETTINGS_LIGHTING_CAST_SHADOWS_2_DESC", "Whether point/spot/area lights may render shadow maps."), [&]() {
                    if (ImGui::Checkbox("##AdditionalLightsCastShadows", &graphics.additionalLightsCastShadows)) {
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
            }
        }

        if (DrawSettingSection("[L]", Loc::T("SETTINGS_LIGHTING_MODEL", "Lighting Model"), Loc::T("SETTINGS_LIGHTING_MODEL_DESC", "Choose which surface-lighting contributions the renderer evaluates."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Lighting Model", "Specular", "phong ambient diffuse highlights reflections")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_SPECULAR", "Specular"), Loc::T("SETTINGS_LIGHTING_SPECULAR_DESC", "Adds view-dependent highlights and reflection casts. Disable it to render ambient and diffuse lighting only."), [&]() {
                    if (ImGui::Checkbox("##LightingSpecular", &graphics.specularEnabled)) {
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
            }
        }

        if (DrawSettingSection("[S]", Loc::T("SETTINGS_LIGHTING_SHADOWS", "Shadows"), Loc::T("SETTINGS_LIGHTING_SHADOWS_DESC", "Project-wide shadow behavior."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Shadows", "Max Distance", "shadow distance far")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_MAX_DISTANCE", "Max Distance"), Loc::T("SETTINGS_LIGHTING_MAX_DISTANCE_DESC", "Directional shadows only cover this far from the camera. 0 follows the camera far plane; smaller values sharpen shadows."), [&]() {
                    if (ImGui::DragFloat("##ShadowMaxDistance", &graphics.shadowMaxDistance, 1.0f, 0.0f, 100000.0f, graphics.shadowMaxDistance <= 0.0f ? "Unlimited" : "%.0f")) {
                        graphics.shadowMaxDistance = std::max(0.0f, graphics.shadowMaxDistance);
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Shadows", "Soft Shadows", "pcf soft hard")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_SOFT_SHADOWS", "Soft Shadows"), Loc::T("SETTINGS_LIGHTING_SOFT_SHADOWS_DESC", "Allows the softened shadow mode. Off forces hard shadows even on lights configured as soft."), [&]() {
                    if (ImGui::Checkbox("##LightingSoftShadows", &graphics.softShadows)) {
                        applyProjectGraphicsToRenderer();
                        return true;
                    }
                    return false;
                });
            }
        }

        ImGui::EndDisabled();

        if (has2DWorldPackage() &&
            DrawSettingSection("[2]", Loc::T("SETTINGS_LIGHTING_2_D_LIGHTING", "2D Lighting"), Loc::T("SETTINGS_LIGHTING_2_D_LIGHTING_DESC", "Lighting for the 2D world. Shown because this project has the 2D world package."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "2D Lighting", "Lighting Resolution", "2d light buffer scale resolution performance auto")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_LIGHTING_RESOLUTION", "Lighting Resolution"), Loc::T("SETTINGS_LIGHTING_LIGHTING_RESOLUTION_DESC", "Internal render scale for the 2D light buffers. Auto adapts to output resolution and light count; turn it off to render at exactly this scale. Ships with the game."), [&]() {
                    bool resAuto = ImGui::Checkbox("Auto##Light2DRes2DSection", &buildSettings.light2DLightingResolutionAuto);
                    ImGui::SameLine();
                    bool resSlider = ImGui::SliderFloat("##Light2DRes2DSection", &buildSettings.light2DLightingResolution, 0.25f, 1.0f, "%.2fx");
                    if (resSlider) buildSettings.light2DLightingResolution = std::clamp(buildSettings.light2DLightingResolution, 0.25f, 1.0f);
                    return resAuto || resSlider;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "2D Lighting", "Lighting Filter", "2d light nearest bilinear pixel filter performance")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_LIGHTING_FILTER", "Lighting Filter"), Loc::T("SETTINGS_LIGHTING_LIGHTING_FILTER_DESC", "How the 2D lighting output is filtered. Off is bilinear (smooth); on is nearest, giving crisp pixel-art light and skipping the bilinear taps when sampling the light buffers (a little cheaper per pixel). Ships with the game."), [&]() {
                    return ImGui::Checkbox("Pixel (Nearest)##Light2DPixelFilter2DSection", &buildSettings.light2DLightingPixelFilter);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "2D Lighting", "Lighting Depth", "2d light precision format hdr sdr bandwidth performance")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_LIGHTING_LIGHTING_DEPTH", "Lighting Depth"), Loc::T("SETTINGS_LIGHTING_LIGHTING_DEPTH_DESC", "Internal format of the 2D lighting buffers. Off is HDR RGBA16F; on is SDR RGBA8, which halves the lighting-buffer bandwidth (runs faster) but clamps overbright/additive light at white. Ships with the game."), [&]() {
                    return ImGui::Checkbox("SDR 8-bit##Light2DSDR2DSection", &buildSettings.light2DLightingSDR);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "2D Lighting", "Light Stats Overlay", "2d light debug stats")) {
                DrawSettingRow(Loc::T("SETTINGS_LIGHTING_LIGHT_STATS_OVERLAY", "Light Stats Overlay"), Loc::T("SETTINGS_LIGHTING_LIGHT_STATS_OVERLAY_DESC", "Show the 2D lighting statistics overlay in the viewport."), [&]() {
                    if (ImGui::Checkbox("##Light2DStatsOverlay2DSection", &showLight2DStatsOverlay)) {
                        saveEditorUserSettings();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "2D Lighting", "Active 2D Lights", "count debug lighting")) {
                DrawSettingRow(Loc::T("SETTINGS_LIGHTING_ACTIVE_2_D_LIGHTS", "Active 2D Lights"), Loc::T("SETTINGS_LIGHTING_ACTIVE_2_D_LIGHTS_DESC", "Enabled 2D lights that contributed on the last frame."), [&]() {
                    ImGui::Text("%d", light2DActiveCountLastFrame);
                    return false;
                });
            }
        }

        if (changed) {
            projectManager.currentProject.saveProjectFile();
            projectManager.currentProject.hasUnsavedChanges = true;
        }
        if (buildSettingsChanged) {
            saveBuildSettings();
        }
    } else if (selectedTab == kTagsLayersTab) {
        auto drawNameList = [&](const char* section, std::vector<std::string>& values, const char* defaultName, int maxCount) {
            bool changed = false;
            if (values.empty()) values.push_back(defaultName);
            if (values[0].empty()) values[0] = defaultName;
            for (size_t i = 0; i < values.size(); ++i) {
                if (!ProjectSettingsMatchesSearch(settingsSearch, section, values[i].c_str(), "tags layers names")) continue;
                ++visibleSettingCount;
                ImGui::PushID(static_cast<int>(i));
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer), "%s", values[i].c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 150.0f);
                if (ImGui::InputText("##Name", buffer, sizeof(buffer))) {
                    std::string next = TrimCopy(buffer);
                    if (IsValidProjectNameToken(next) && !HasDuplicateProjectNameToken(values, next, i)) {
                        values[i] = next;
                        changed = true;
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("#%zu", i);
                ImGui::SameLine();
                ImGui::BeginDisabled(i == 0);
                if (ImGui::SmallButton("Up") && i > 1) {
                    std::swap(values[i], values[i - 1]);
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Down") && i + 1 < values.size()) {
                    std::swap(values[i], values[i + 1]);
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove") && i != 0) {
                    values.erase(values.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndDisabled();
                if (!IsValidProjectNameToken(values[i]) || HasDuplicateProjectNameToken(values, values[i], i)) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Invalid");
                }
                ImGui::PopID();
            }
            ImGui::BeginDisabled(static_cast<int>(values.size()) >= maxCount);
            if (ImGui::Button((std::string("+ Add ") + section).c_str())) {
                std::string base = std::string(section) + " " + std::to_string(values.size());
                int suffix = static_cast<int>(values.size());
                while (HasDuplicateProjectNameToken(values, base, SIZE_MAX)) {
                    base = std::string(section) + " " + std::to_string(++suffix);
                }
                values.push_back(base);
                changed = true;
            }
            ImGui::EndDisabled();
            return changed;
        };

        bool changed = false;
        if (DrawSettingSection("[L]", Loc::T("SETTINGS_TAGS_LAYERS", "Layers"), Loc::T("SETTINGS_TAGS_LAYERS_DESC", "Name-based layers with stable internal numeric indices."))) {
            DrawHelpText("Layer references keep serializing as numbers internally. Names only make the editor safer to read.");
            changed |= drawNameList("Layer", projectManager.currentProject.physicsSettings.collisionLayers, "Default", 32);
        }
        if (DrawSettingSection("[T]", Loc::T("SETTINGS_TAGS_TAGS", "Tags"), Loc::T("SETTINGS_TAGS_TAGS_DESC", "Reusable object tags with validation."))) {
            changed |= drawNameList("Tag", projectManager.currentProject.tags, "Untagged", 256);
        }
        if (changed) {
            projectManager.currentProject.saveProjectFile();
            projectManager.currentProject.hasUnsavedChanges = true;
        }
    } else if (selectedTab == kConsoleConfigTab) {
        ProjectConsoleSettings& consoleConfig = projectManager.currentProject.consoleSettings;
        bool changed = false;
        if (DrawSettingSection("[C]", Loc::T("SETTINGS_CONSOLE_CONSOLE_BEHAVIOR", "Console Behavior"), Loc::T("SETTINGS_CONSOLE_CONSOLE_BEHAVIOR_DESC", "How the console appears while editing and testing."))) {
            changed |= DrawSettingRow(Loc::T("SETTINGS_CONSOLE_CONSOLE_MODE", "Console Mode"), Loc::T("SETTINGS_CONSOLE_CONSOLE_MODE_DESC", "Choose how the console should appear by default."), [&]() {
                int mode = static_cast<int>(consoleConfig.mode);
                const char* modes[] = { "Docked mini-button", "Floating window" };
                if (ImGui::Combo("##ConsoleMode", &mode, modes, IM_ARRAYSIZE(modes))) {
                    consoleConfig.mode = static_cast<ProjectConsoleMode>(std::clamp(mode, 0, 1));
                    showConsole = true;
                    consolePanelExpanded = consoleConfig.alwaysOpenOnLaunch;
                    return true;
                }
                return false;
            });
            changed |= DrawSettingRow(Loc::T("SETTINGS_CONSOLE_ALWAYS_OPEN_ON_LAUNCH", "Always Open on Launch"), Loc::T("SETTINGS_CONSOLE_ALWAYS_OPEN_ON_LAUNCH_DESC", "Show the console automatically when the editor opens this project."), [&]() {
                if (ImGui::Checkbox("##ConsoleAlwaysOpenOnLaunch", &consoleConfig.alwaysOpenOnLaunch)) {
                    if (consoleConfig.alwaysOpenOnLaunch) {
                        showConsole = true;
                        consolePanelExpanded = true;
                    }
                    return true;
                }
                return false;
            });
            changed |= DrawSettingRow(Loc::T("SETTINGS_CONSOLE_OPEN_ONLY_ON_ERRORS", "Open Only on Errors"), Loc::T("SETTINGS_CONSOLE_OPEN_ONLY_ON_ERRORS_DESC", "Keep the console quiet until errors happen."), [&]() {
                return ImGui::Checkbox("##ConsoleOpenOnlyOnErrors", &consoleConfig.openOnlyOnErrors);
            });
            changed |= DrawSettingRow(Loc::T("SETTINGS_CONSOLE_CONSOLE_TONE", "Console Tone"), Loc::T("SETTINGS_CONSOLE_CONSOLE_TONE_DESC", "Choose expressive or professional console message wording."), [&]() {
                int tone = static_cast<int>(consoleConfig.tone);
                const char* tones[] = { "Fun", "Concise" };
                if (ImGui::Combo("##ConsoleTone", &tone, tones, IM_ARRAYSIZE(tones))) {
                    consoleConfig.tone = static_cast<ProjectConsoleTone>(std::clamp(tone, 0, 1));
                    return true;
                }
                return false;
            });
        }
        if (changed) projectManager.currentProject.saveProjectFile();
    } else if (selectedTab == kAudioConfigTab) {
        bool editorSettingsChanged = false;

        if (sectionVisible("Audio", ProjectSettingsVisibilityMode::Simple) &&
            DrawSettingSection("[A]", Loc::T("SETTINGS_AUDIO_AUDIO", "Audio"), Loc::T("SETTINGS_AUDIO_AUDIO_DESC", "Editor preview and feedback sounds."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Audio", "Feedback Sounds", "click error editor sounds")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_AUDIO_FEEDBACK_SOUNDS", "Feedback Sounds"), Loc::T("SETTINGS_AUDIO_FEEDBACK_SOUNDS_DESC", "Plays short editor sounds for clicks and status feedback."), [&]() {
                    return ImGui::Checkbox("##FeedbackSounds", &feedbackSoundsEnabled);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Audio", "Preview Volume", "asset browser audio")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_AUDIO_PREVIEW_VOLUME", "Preview Volume"), Loc::T("SETTINGS_AUDIO_PREVIEW_VOLUME_DESC", "Volume for audio previews in editor tools."), [&]() {
                    return ImGui::SliderFloat("##AudioPreviewVolume", &audioPreviewVolume, 0.0f, 2.0f, "%.2f");
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Audio", "Sound Categories", "click error other feedback")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_AUDIO_SOUND_CATEGORIES", "Sound Categories"), Loc::T("SETTINGS_AUDIO_SOUND_CATEGORIES_DESC", "Developer toggles for individual editor sound groups."), [&]() {
                    bool changed = false;
                    changed |= ImGui::Checkbox("Click##FeedbackClickSounds", &feedbackClickSoundsEnabled);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("Error##FeedbackErrorSounds", &feedbackErrorSoundsEnabled);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("Other##FeedbackOtherSounds", &feedbackOtherSoundsEnabled);
                    return changed;
                });
            }
        }

        if (settingsSearch[0] != '\0' && visibleSettingCount == 0) {
            ImGui::TextDisabled("No audio settings match this search.");
        }

        if (editorSettingsChanged) {
            saveEditorUserSettings();
        }
    } else if (selectedTab == kPlayerConfigTab) {
        ProjectPlayerSettings& player = projectManager.currentProject.playerSettings;
        bool changed = false;
        auto drawStringSetting = [&](const char* label, const char* helper, std::string& value, size_t bufferSize = 256) {
            return DrawSettingRow(label, helper, [&]() {
                std::vector<char> buffer(bufferSize, '\0');
                std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
                if (ImGui::InputText("##Text", buffer.data(), buffer.size())) {
                    value = TrimCopy(buffer.data());
                    return true;
                }
                return false;
            });
        };
        if (DrawSettingSection("[A]", Loc::T("SETTINGS_PLAYER_APPLICATION", "Application"), Loc::T("SETTINGS_PLAYER_APPLICATION_DESC", "Player identity and startup scene."))) {
            changed |= drawStringSetting("Product Name", "Name shown for exported players.", player.productName);
            changed |= drawStringSetting("Company Name", "Company or studio name used by builds and save paths.", player.companyName);
            changed |= DrawSettingRow(Loc::T("SETTINGS_PLAYER_DEFAULT_SCENE", "Default Scene"), Loc::T("SETTINGS_PLAYER_DEFAULT_SCENE_DESC", "Scene loaded first by the player."), [&]() {
                auto scenes = projectManager.currentProject.getSceneList();
                std::vector<const char*> sceneNames;
                sceneNames.reserve(scenes.size());
                int current = 0;
                for (size_t i = 0; i < scenes.size(); ++i) {
                    sceneNames.push_back(scenes[i].c_str());
                    if (scenes[i] == player.defaultScene) current = static_cast<int>(i);
                }
                if (sceneNames.empty()) {
                    ImGui::TextDisabled("No scenes available");
                    return false;
                }
                if (ImGui::Combo("##DefaultScene", &current, sceneNames.data(), static_cast<int>(sceneNames.size()))) {
                    player.defaultScene = scenes[static_cast<size_t>(current)];
                    projectManager.currentProject.currentSceneName = player.defaultScene;
                    return true;
                }
                return false;
            });
            changed |= drawStringSetting("Application Icon", "Path to the icon asset used by supported build targets.", player.applicationIconPath, 512);
        }
        if (drawSettingSectionIcon("Resources/Engine-Root/Project Settings/Dropdowns/Player Config/Window and cursor.png",
                                   "[W]", Loc::T("SETTINGS_PLAYER_WINDOW_AND_CURSOR", "Window and Cursor"), Loc::T("SETTINGS_PLAYER_WINDOW_AND_CURSOR_DESC", "Startup resolution, fullscreen mode, and cursor defaults."))) {
            changed |= DrawSettingRow(Loc::T("SETTINGS_PLAYER_STARTUP_RESOLUTION", "Startup Resolution"), Loc::T("SETTINGS_PLAYER_STARTUP_RESOLUTION_DESC", "Default player window size."), [&]() {
                bool rowChanged = ImGui::DragInt("Width##PlayerStartupWidth", &player.startupWidth, 1.0f, 64, 8192);
                rowChanged |= ImGui::DragInt("Height##PlayerStartupHeight", &player.startupHeight, 1.0f, 64, 8192);
                if (rowChanged) {
                    player.startupWidth = std::clamp(player.startupWidth, 64, 8192);
                    player.startupHeight = std::clamp(player.startupHeight, 64, 8192);
                }
                return rowChanged;
            });
            changed |= DrawSettingRow(Loc::T("SETTINGS_PLAYER_FULLSCREEN_STARTUP", "Fullscreen Startup"), Loc::T("SETTINGS_PLAYER_FULLSCREEN_STARTUP_DESC", "Launch the player fullscreen by default."), [&]() {
                return ImGui::Checkbox("##PlayerFullscreenStartup", &player.fullscreenStartup);
            });
            changed |= DrawSettingRow(Loc::T("SETTINGS_PLAYER_NATIVE_DISPLAY_RESOLUTION", "Native Display Resolution"),
                                      Loc::T("SETTINGS_PLAYER_NATIVE_DISPLAY_RESOLUTION_DESC", "Render at the device's actual display surface size instead of the Startup Resolution above. On Android this is the EGL surface size (e.g. tablet's native res); on desktop, the GLFW window framebuffer. Recommended for mobile builds so 16:9 content doesn't get stretched into a 16:10 panel."),
                                      [&]() {
                return ImGui::Checkbox("##PlayerNativeDisplayResolution", &player.nativeDisplayResolution);
            });
            changed |= DrawSettingRow(Loc::T("SETTINGS_PLAYER_CURSOR_DEFAULTS", "Cursor Defaults"), Loc::T("SETTINGS_PLAYER_CURSOR_DEFAULTS_DESC", "Initial cursor lock and visibility."), [&]() {
                bool rowChanged = ImGui::Checkbox("Locked##CursorLocked", &player.cursorLocked);
                ImGui::SameLine();
                rowChanged |= ImGui::Checkbox("Visible##CursorVisible", &player.cursorVisible);
                return rowChanged;
            });
        }
        if (DrawSettingSection("[B]", Loc::T("SETTINGS_PLAYER_BUILD_DEFAULTS", "Build Defaults"), Loc::T("SETTINGS_PLAYER_BUILD_DEFAULTS_DESC", "Default target and save-data behavior for player builds."))) {
            changed |= drawStringSetting("Build Target", "Default build target used when opening build settings.", player.buildTarget);
            changed |= drawStringSetting("Save Data Path", "How the runtime groups persistent save data.", player.saveDataPathBehavior);
        }
        if (changed) {
            if (player.productName.empty()) player.productName = projectManager.currentProject.name;
            if (player.companyName.empty()) player.companyName = "DefaultCompany";
            projectManager.currentProject.saveProjectFile();
        }
    } else if (selectedTab == kEditorTab) {
        bool editorSettingsChanged = false;
        bool buildSettingsChanged = false;

        if (sectionVisible("Project", ProjectSettingsVisibilityMode::Simple) &&
            drawSettingSectionIcon("Resources/Engine-Root/Project Settings/Dropdowns/Editor/Project.png",
                                   "[P]", Loc::T("SETTINGS_EDITOR_PROJECT", "Project"), Loc::T("SETTINGS_EDITOR_PROJECT_DESC", "Project identity and the broad mode this project uses."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Project", "Project Mode", "pipeline 2d 3d 2.5d")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_PROJECT_MODE", "Project Mode"), Loc::T("SETTINGS_EDITOR_PROJECT_MODE_DESC", "Choose the default editing pipeline for this project."), [&]() {
                    int pipelineIndex = ProjectPipelineToUiIndex(projectManager.currentProject.pipeline);
                    const char* pipelineOptions[] = { "3D Pipeline", "2.5D Pipeline (Experimental)", "2D Pipeline" };
                    if (ImGui::Combo("##ProjectPipelineMode", &pipelineIndex, pipelineOptions, IM_ARRAYSIZE(pipelineOptions))) {
                        projectManager.currentProject.pipeline = ProjectPipelineFromUiIndex(pipelineIndex);
                        projectManager.currentProject.saveProjectFile();
                        applyProjectPipelineDefaults(false);
                        projectManager.currentProject.hasUnsavedChanges = true;
                        return true;
                    }
                    return false;
                });
            }

            if ((projectManager.currentProject.pipeline == ProjectPipeline::Pipeline2D || has2DWorldPackage()) &&
                visible(ProjectSettingsVisibilityMode::Simple, "Project", "Pixel Grid Snap", "2d snap grid")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_PIXEL_GRID_SNAP", "Pixel Grid Snap"), Loc::T("SETTINGS_EDITOR_PIXEL_GRID_SNAP_DESC", "Keeps 2D object movement aligned to whole pixels."), [&]() {
                    return ImGui::Checkbox("##PixelGridSnap", &pixelGridSnapEnabled);
                });
            }

            if ((projectManager.currentProject.pipeline == ProjectPipeline::Pipeline2D || has2DWorldPackage()) &&
                visible(ProjectSettingsVisibilityMode::Advanced, "Project", "Snap Step", "2d snap grid pixels")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_SNAP_STEP", "Snap Step"), Loc::T("SETTINGS_EDITOR_SNAP_STEP_DESC", "Pixel size used by the 2D grid snap."), [&]() {
                    ImGui::BeginDisabled(!pixelGridSnapEnabled);
                    const bool changed = ImGui::DragInt("##PixelGridSnapStep", &pixelGridSnapStep, 1.0f, 1, 64);
                    ImGui::EndDisabled();
                    if (changed) pixelGridSnapStep = std::clamp(pixelGridSnapStep, 1, 64);
                    return changed;
                });
            }
        }

        if (sectionVisible("Player / Viewport", ProjectSettingsVisibilityMode::Simple) &&
            drawSettingSectionIcon("Resources/Engine-Root/Project Settings/Dropdowns/Editor/Player and viewport.png",
                                   "[V]", Loc::T("SETTINGS_EDITOR_PLAYER_VIEWPORT", "Player / Viewport"), Loc::T("SETTINGS_EDITOR_PLAYER_VIEWPORT_DESC", "Preview and viewport behavior."))) {
            const char* resolutionOptions[] = { "Default (1280x720)", "1080p", "720p", "1440p", "Custom" };
            if (gameViewportResolutionIndex < 0 || gameViewportResolutionIndex >= static_cast<int>(IM_ARRAYSIZE(resolutionOptions))) {
                gameViewportResolutionIndex = 0;
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Player / Viewport", "Preview Resolution", "internal render size performance retro")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_PREVIEW_RESOLUTION", "Preview Resolution"), Loc::T("SETTINGS_EDITOR_PREVIEW_RESOLUTION_DESC", "Controls the internal preview render size. Lower values improve performance or create a retro look."), [&]() {
                    int resolutionIndex = gameViewportResolutionIndex;
                    if (ImGui::Combo("##PreviewResolution", &resolutionIndex, resolutionOptions, IM_ARRAYSIZE(resolutionOptions))) {
                        gameViewportResolutionIndex = resolutionIndex;
                        return true;
                    }
                    return false;
                });
            }
            if (gameViewportResolutionIndex == 4 &&
                visible(ProjectSettingsVisibilityMode::Advanced, "Player / Viewport", "Custom Preview Size", "width height resolution")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_CUSTOM_PREVIEW_SIZE", "Custom Preview Size"), Loc::T("SETTINGS_EDITOR_CUSTOM_PREVIEW_SIZE_DESC", "Used only when Preview Resolution is Custom."), [&]() {
                    bool changed = false;
                    ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
                    if (ImGui::DragInt("##PreviewCustomWidth", &gameViewportCustomWidth, 1.0f, 64, 8192)) {
                        gameViewportCustomWidth = std::clamp(gameViewportCustomWidth, 64, 8192);
                        changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragInt("##PreviewCustomHeight", &gameViewportCustomHeight, 1.0f, 64, 8192)) {
                        gameViewportCustomHeight = std::clamp(gameViewportCustomHeight, 64, 8192);
                        changed = true;
                    }
                    return changed;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Player / Viewport", "Auto Fit Preview", "zoom scale")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_AUTO_FIT_PREVIEW", "Auto Fit Preview"), Loc::T("SETTINGS_EDITOR_AUTO_FIT_PREVIEW_DESC", "Automatically fits the game preview inside its panel."), [&]() {
                    return ImGui::Checkbox("##AutoFitPreview", &gameViewportAutoFit);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Player / Viewport", "Preview Zoom", "scale")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_PREVIEW_ZOOM", "Preview Zoom"), Loc::T("SETTINGS_EDITOR_PREVIEW_ZOOM_DESC", "Manual zoom used when Auto Fit Preview is off."), [&]() {
                    ImGui::BeginDisabled(gameViewportAutoFit);
                    float zoomPercent = gameViewportZoom * 100.0f;
                    const bool changed = ImGui::SliderFloat("##PreviewZoom", &zoomPercent, 100.0f, 800.0f, "%.0f%%");
                    ImGui::EndDisabled();
                    if (changed) gameViewportZoom = std::clamp(zoomPercent / 100.0f, 1.0f, 8.0f);
                    return changed;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Player / Viewport", "Canvas Guides", "ui overlay guides")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_CANVAS_GUIDES", "Canvas Guides"), Loc::T("SETTINGS_EDITOR_CANVAS_GUIDES_DESC", "Shows reference guides for UI layout while editing."), [&]() {
                    return ImGui::Checkbox("##CanvasGuides", &showCanvasOverlay);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Player / Viewport", "UI Canvas Preview", "ui canvas preview selection overlay")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_UI_CANVAS_PREVIEW", "UI Canvas Preview"), Loc::T("SETTINGS_EDITOR_UI_CANVAS_PREVIEW_DESC", "Shows UI canvas previews in editor viewports while not playing."), [&]() {
                    return ImGui::Checkbox("##UICanvasPreview", &uiCanvasPreviewEnabled);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Player / Viewport", "UI World Grid", "world ui grid overlay")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_UI_WORLD_GRID", "UI World Grid"), Loc::T("SETTINGS_EDITOR_UI_WORLD_GRID_DESC", "Shows the grid while editing world-space UI."), [&]() {
                    return ImGui::Checkbox("##UIWorldGrid", &showUIWorldGrid);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Player / Viewport", "Viewport Toolbar Corner", "toolbar position")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_TOOLBAR_CORNER", "Toolbar Corner"), Loc::T("SETTINGS_EDITOR_TOOLBAR_CORNER_DESC", "Moves the scene viewport toolbar to a different corner."), [&]() {
                    const char* toolbarCornerOptions[] = { "Bottom Left", "Bottom Right", "Top Left", "Top Right" };
                    int toolbarCornerIndex = static_cast<int>(sceneViewportToolbarCorner);
                    toolbarCornerIndex = std::clamp(toolbarCornerIndex, 0, static_cast<int>(IM_ARRAYSIZE(toolbarCornerOptions)) - 1);
                    if (ImGui::Combo("##ViewportToolbarCorner", &toolbarCornerIndex, toolbarCornerOptions, IM_ARRAYSIZE(toolbarCornerOptions))) {
                        sceneViewportToolbarCorner = static_cast<ViewportToolbarCorner>(toolbarCornerIndex);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Player / Viewport", "Viewport Hint Overlay", "hints help overlay")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_VIEWPORT_HINTS", "Viewport Hints"), Loc::T("SETTINGS_EDITOR_VIEWPORT_HINTS_DESC", "Shows short helper hints in viewport toolbars."), [&]() {
                    return ImGui::Checkbox("##ViewportHints", &showViewportHintOverlay);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Player / Viewport", "Viewport Profiler", "performance overlay profiler")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_VIEWPORT_PROFILER", "Viewport Profiler"), Loc::T("SETTINGS_EDITOR_VIEWPORT_PROFILER_DESC", "Shows runtime timing information over the preview."), [&]() {
                    return ImGui::Checkbox("##ViewportProfiler", &showGameProfiler);
                });
            }
        }

        if (sectionVisible("Build", ProjectSettingsVisibilityMode::Simple) &&
            drawSettingSectionIcon("Resources/Engine-Root/Project Settings/Dropdowns/Editor/ModuCPP Logo.png",
                                   "[B]", Loc::T("SETTINGS_EDITOR_BUILD", "Build"), Loc::T("SETTINGS_EDITOR_BUILD_DESC", "Build profile shortcuts. Full controls remain in the Build tab."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Build", "Platform", "target windows linux android")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_PLATFORM", "Platform"), Loc::T("SETTINGS_EDITOR_PLATFORM_DESC", "Target platform used by the build profile."), [&]() {
                    int targetIndex = buildPlatformIndex(buildSettings.platform);
                    if (ImGui::Combo("##BuildPlatformQuick", &targetIndex,
                                     kBuildPlatformLabels, kBuildPlatformCount)) {
                        BuildPlatform newPlatform = buildPlatformFromIndex(targetIndex);
                        if (newPlatform != buildSettings.platform) {
                            buildSettings.platform = newPlatform;
                            int archCount = 0;
                            const BuildArchitectureInfo* archList = buildArchitecturesForPlatform(newPlatform, archCount);
                            bool valid = false;
                            for (int i = 0; i < archCount; ++i) {
                                if (buildSettings.architecture == archList[i].serializedName) { valid = true; break; }
                            }
                            if (!valid) buildSettings.architecture = archList[0].serializedName;
                            return true;
                        }
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Build", "Development Build", "debug build profile")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_DEVELOPMENT_BUILD", "Development Build"), Loc::T("SETTINGS_EDITOR_DEVELOPMENT_BUILD_DESC", "Build with development flags enabled."), [&]() {
                    return ImGui::Checkbox("##DevelopmentBuildQuick", &buildSettings.developmentBuild);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Build", "Profiling", "profiler script debugging deep profiling")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_PROFILING", "Profiling"), Loc::T("SETTINGS_EDITOR_PROFILING_DESC", "Developer build diagnostics and profiling options."), [&]() {
                    bool changed = false;
                    changed |= ImGui::Checkbox("Script Debug##ScriptDebuggingQuick", &buildSettings.scriptDebugging);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("Profiler##AutoProfilerQuick", &buildSettings.autoConnectProfiler);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("Deep##DeepProfilingQuick", &buildSettings.deepProfiling);
                    return changed;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Build", "Open Build Settings", "advanced build window")) {
                DrawSettingRow(Loc::T("SETTINGS_EDITOR_MORE_BUILD_OPTIONS", "More Build Options"), Loc::T("SETTINGS_EDITOR_MORE_BUILD_OPTIONS_DESC", "Open the dedicated Build tab for scenes, packaging, and export."), [&]() {
                    if (ImGui::Button("Open Build Tab")) {
                        selectedTab = kBuildTab;
                        return true;
                    }
                    return false;
                });
            }
        }

        if (sectionVisible("Editor", ProjectSettingsVisibilityMode::Simple) &&
            drawSettingSectionIcon("Resources/Engine-Root/Project Settings/Dropdowns/Editor/Editor.png",
                                   "[E]", Loc::T("SETTINGS_EDITOR_EDITOR", "Editor"), Loc::T("SETTINGS_EDITOR_EDITOR_DESC", "Camera movement and editor-only viewport aids."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Editor", "Move Speed", "camera navigation")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_MOVE_SPEED", "Move Speed"), Loc::T("SETTINGS_EDITOR_MOVE_SPEED_DESC", "Base speed for editor camera movement."), [&]() {
                    if (ImGui::DragFloat("##CameraMoveSpeed", &camera.moveSpeed, 0.1f, 0.01f, 100.0f, "%.2f")) {
                        camera.moveSpeed = std::max(0.01f, camera.moveSpeed);
                        camera.sprintSpeed = std::max(camera.moveSpeed, camera.sprintSpeed);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Editor", "Sprint Speed", "camera navigation")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_SPRINT_SPEED", "Sprint Speed"), Loc::T("SETTINGS_EDITOR_SPRINT_SPEED_DESC", "Fast movement speed while sprinting in the editor camera."), [&]() {
                    if (ImGui::DragFloat("##CameraSprintSpeed", &camera.sprintSpeed, 0.1f, 0.01f, 200.0f, "%.2f")) {
                        camera.sprintSpeed = std::max(camera.moveSpeed, camera.sprintSpeed);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Editor", "Smooth Movement", "camera smoothing")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_SMOOTH_MOVEMENT", "Smooth Movement"), Loc::T("SETTINGS_EDITOR_SMOOTH_MOVEMENT_DESC", "Softens editor camera starts and stops."), [&]() {
                    return ImGui::Checkbox("##CameraSmoothMovement", &camera.smoothMovement);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Editor", "Acceleration", "camera smoothing speed")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_ACCELERATION", "Acceleration"), Loc::T("SETTINGS_EDITOR_ACCELERATION_DESC", "How quickly smooth camera movement reaches full speed."), [&]() {
                    ImGui::BeginDisabled(!camera.smoothMovement);
                    const bool changed = ImGui::DragFloat("##CameraAcceleration", &camera.acceleration, 0.1f, 0.1f, 200.0f, "%.2f");
                    ImGui::EndDisabled();
                    if (changed) camera.acceleration = std::max(0.1f, camera.acceleration);
                    return changed;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Editor", "Mouse Sensitivity", "camera look")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_MOUSE_SENSITIVITY", "Mouse Sensitivity"), Loc::T("SETTINGS_EDITOR_MOUSE_SENSITIVITY_DESC", "Look sensitivity for the editor camera."), [&]() {
                    if (ImGui::SliderFloat("##CameraMouseSensitivity", &camera.mouseSensitivity, 0.001f, 1.0f, "%.3f")) {
                        camera.mouseSensitivity = std::clamp(camera.mouseSensitivity, 0.001f, 1.0f);
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Editor", "Camera Projection", "fov near far clip")) {
                buildSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_CAMERA_PROJECTION", "Camera Projection"), Loc::T("SETTINGS_EDITOR_CAMERA_PROJECTION_DESC", "Developer controls for the editor camera projection."), [&]() {
                    bool changed = false;
                    changed |= ImGui::SliderFloat("FOV##EditorCameraFov", &buildSettings.editorCameraFov, 20.0f, 140.0f, "%.1f");
                    changed |= ImGui::DragFloat("Near##EditorCameraNear", &buildSettings.editorCameraNear, 0.01f, 0.01f, buildSettings.editorCameraFar - 0.01f, "%.3f");
                    changed |= ImGui::DragFloat("Far##EditorCameraFar", &buildSettings.editorCameraFar, 0.1f, buildSettings.editorCameraNear + 0.05f, 5000.0f, "%.1f");
                    if (changed) {
                        buildSettings.editorCameraNear = std::max(0.01f, std::min(buildSettings.editorCameraNear, buildSettings.editorCameraFar - 0.01f));
                        buildSettings.editorCameraFar = std::max(buildSettings.editorCameraNear + 0.05f, buildSettings.editorCameraFar);
                    }
                    return changed;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Editor", "Hierarchy Texture Preview", "hierarchy texture thumbnails")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_HIERARCHY_TEXTURE_PREVIEW", "Hierarchy Texture Preview"), Loc::T("SETTINGS_EDITOR_HIERARCHY_TEXTURE_PREVIEW_DESC", "Shows small material texture thumbnails in the Hierarchy panel."), [&]() {
                    return ImGui::Checkbox("##HierarchyTexturePreview", &hierarchyShowTexturePreview);
                });
            }
        }

        if (projectManager.currentProject.pipeline == ProjectPipeline::Pipeline25D &&
            sectionVisible("2.5D Presentation", ProjectSettingsVisibilityMode::Advanced) &&
            DrawSettingSection("[2.5D]", Loc::T("SETTINGS_EDITOR_2_5_D_PRESENTATION", "2.5D Presentation"), Loc::T("SETTINGS_EDITOR_2_5_D_PRESENTATION_DESC", "Technical presentation controls for TM/vexel projects."), false)) {
            auto& tmPresentation = tmOpenGLRenderer.getPresentationSettings();
            if (visible(ProjectSettingsVisibilityMode::Advanced, "2.5D Presentation", "Fake 3D", "glue mode7 lowres tile retro fake")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_FAKE_3_D", "Fake 3D"), Loc::T("SETTINGS_EDITOR_FAKE_3_D_DESC", "Glue3D-style low-res mode7 presentation (tile+seg+m7,tsm)."), [&]() {
                    bool changed = ImGui::Checkbox("##TMFake3D", &tmPresentation.fake3DEnabled);
                    ImGui::BeginDisabled(!tmPresentation.fake3DEnabled);
                    changed |= ImGui::DragInt("Lines##TMFake3DHeight", &tmPresentation.fake3DInternalHeight, 1.0f, 64, 2160);
                    changed |= ImGui::Checkbox("Point Sampling##TMFake3DPoint", &tmPresentation.fake3DPointSampling);
                    changed |= ImGui::Checkbox("Flat Shading##TMFake3DFlat", &tmPresentation.fake3DFlatShading);
                    changed |= ImGui::DragInt("Shade Levels##TMFake3DShadeLevels", &tmPresentation.fake3DShadeLevels, 0.1f, 0, 16);
                    changed |= ImGui::Checkbox("Affine Textures##TMFake3DAffine", &tmPresentation.fake3DAffineTextures);
                    ImGui::BeginDisabled(!tmPresentation.fake3DAffineTextures);
                    changed |= ImGui::SliderFloat("Warp##TMFake3DAffineStrength", &tmPresentation.fake3DAffineStrength, 0.0f, 1.0f, "%.2f");
                    ImGui::EndDisabled();
                    ImGui::EndDisabled();
                    if (changed) {
                        tmPresentation.fake3DInternalHeight = std::clamp(tmPresentation.fake3DInternalHeight, 64, 2160);
                        tmPresentation.fake3DShadeLevels = std::clamp(tmPresentation.fake3DShadeLevels, 0, 16);
                        tmPresentation.fake3DAffineStrength = std::clamp(tmPresentation.fake3DAffineStrength, 0.0f, 1.0f);
                    }
                    return changed;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "2.5D Presentation", "Pitch Stretch", "tm vexel look pitch")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_PITCH_STRETCH", "Pitch Stretch"), Loc::T("SETTINGS_EDITOR_PITCH_STRETCH_DESC", "Adjusts how 2.5D art responds to camera pitch."), [&]() {
                    return ImGui::Checkbox("##TMPitchStretch", &tmPresentation.lookPitchStretchEnabled);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "2.5D Presentation", "Pitch Strength", "stretch compress shear")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_PITCH_STRENGTH", "Pitch Strength"), Loc::T("SETTINGS_EDITOR_PITCH_STRENGTH_DESC", "Internal stretch, compression, and shear tuning."), [&]() {
                    bool changed = false;
                    ImGui::BeginDisabled(!tmPresentation.lookPitchStretchEnabled);
                    changed |= ImGui::DragFloat("Stretch##TMPitchStretchStrength", &tmPresentation.lookPitchStretchStrength, 0.01f, 0.0f, 1.5f, "%.2f");
                    changed |= ImGui::DragFloat("Compress##TMPitchCompressStrength", &tmPresentation.lookPitchCompressStrength, 0.01f, 0.0f, 1.5f, "%.2f");
                    changed |= ImGui::DragFloat("Shear##TMPitchShearStrength", &tmPresentation.lookPitchShearStrength, 0.01f, 0.0f, 1.0f, "%.2f");
                    changed |= ImGui::DragFloat("Curve##TMPitchCurve", &tmPresentation.lookPitchCurve, 0.01f, 0.1f, 4.0f, "%.2f");
                    changed |= ImGui::DragFloat("Depth Range##TMPitchDepthRange", &tmPresentation.lookPitchDepthRange, 0.25f, 1.0f, 256.0f, "%.1f");
                    changed |= ImGui::DragFloatRange2("Pitch Range##TMPitchRange",
                                                      &tmPresentation.presentationPitchMinDegrees,
                                                      &tmPresentation.presentationPitchMaxDegrees,
                                                      0.5f, -89.0f, 89.0f, "%.0f deg");
                    ImGui::EndDisabled();
                    if (changed) {
                        tmPresentation.lookPitchStretchStrength = std::clamp(tmPresentation.lookPitchStretchStrength, 0.0f, 1.5f);
                        tmPresentation.lookPitchCompressStrength = std::clamp(tmPresentation.lookPitchCompressStrength, 0.0f, 1.5f);
                        tmPresentation.lookPitchShearStrength = std::clamp(tmPresentation.lookPitchShearStrength, 0.0f, 1.0f);
                        tmPresentation.lookPitchCurve = std::clamp(tmPresentation.lookPitchCurve, 0.1f, 4.0f);
                        tmPresentation.lookPitchDepthRange = std::clamp(tmPresentation.lookPitchDepthRange, 1.0f, 256.0f);
                        tmPresentation.presentationPitchMinDegrees = std::clamp(tmPresentation.presentationPitchMinDegrees, -89.0f, 89.0f);
                        tmPresentation.presentationPitchMaxDegrees = std::clamp(tmPresentation.presentationPitchMaxDegrees,
                                                                                tmPresentation.presentationPitchMinDegrees, 89.0f);
                    }
                    return changed;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "2.5D Presentation", "Snap Presets", "precision retro snap")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_SNAP_PRESETS", "Snap Presets"), Loc::T("SETTINGS_EDITOR_SNAP_PRESETS_DESC", "Switch between clean precision and retro snapped presentation."), [&]() {
                    bool changed = false;
                    if (ImGui::Button("High Precision")) {
                        tmPresentation.presentationSnapEnabled = false;
                        tmPresentation.cameraRelativeSnapEnabled = false;
                        tmPresentation.vertexSnapEnabled = false;
                        tmPresentation.screenSnapEnabled = false;
                        changed = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Retro Snap")) {
                        tmPresentation.presentationSnapEnabled = true;
                        tmPresentation.presentationSnapStep = 0.125f;
                        tmPresentation.cameraRelativeSnapEnabled = true;
                        tmPresentation.cameraRelativeSnapStep = 0.125f;
                        tmPresentation.vertexSnapEnabled = true;
                        tmPresentation.vertexSnapStep = 0.0625f;
                        tmPresentation.screenSnapEnabled = true;
                        tmPresentation.screenSnapStep = 2.0f;
                        changed = true;
                    }
                    return changed;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "2.5D Presentation", "Snap Details", "world camera vertex screen snap steps")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_SNAP_DETAILS", "Snap Details"), Loc::T("SETTINGS_EDITOR_SNAP_DETAILS_DESC", "Internal snap toggles and step sizes for the 2.5D presentation layer."), [&]() {
                    bool changed = false;
                    changed |= ImGui::Checkbox("World##TMPresentationWorldSnap", &tmPresentation.presentationSnapEnabled);
                    ImGui::BeginDisabled(!tmPresentation.presentationSnapEnabled);
                    changed |= ImGui::DragFloat("World Step##TMPresentationWorldSnapStep", &tmPresentation.presentationSnapStep, 0.005f, 0.001f, 8.0f, "%.3f");
                    ImGui::EndDisabled();
                    changed |= ImGui::Checkbox("Camera##TMPresentationCameraSnap", &tmPresentation.cameraRelativeSnapEnabled);
                    ImGui::BeginDisabled(!tmPresentation.cameraRelativeSnapEnabled);
                    changed |= ImGui::DragFloat("Camera Step##TMPresentationCameraSnapStep", &tmPresentation.cameraRelativeSnapStep, 0.005f, 0.001f, 8.0f, "%.3f");
                    ImGui::EndDisabled();
                    changed |= ImGui::Checkbox("Vertex##TMPresentationVertexSnap", &tmPresentation.vertexSnapEnabled);
                    ImGui::BeginDisabled(!tmPresentation.vertexSnapEnabled);
                    changed |= ImGui::DragFloat("Vertex Step##TMPresentationVertexSnapStep", &tmPresentation.vertexSnapStep, 0.0025f, 0.0005f, 4.0f, "%.4f");
                    ImGui::EndDisabled();
                    changed |= ImGui::Checkbox("Screen##TMPresentationScreenSnap", &tmPresentation.screenSnapEnabled);
                    ImGui::BeginDisabled(!tmPresentation.screenSnapEnabled);
                    changed |= ImGui::DragFloat("Screen Step##TMPresentationScreenSnapStep", &tmPresentation.screenSnapStep, 0.25f, 0.25f, 16.0f, "%.2f");
                    ImGui::EndDisabled();
                    if (changed) {
                        tmPresentation.presentationSnapStep = std::clamp(tmPresentation.presentationSnapStep, 0.001f, 8.0f);
                        tmPresentation.cameraRelativeSnapStep = std::clamp(tmPresentation.cameraRelativeSnapStep, 0.001f, 8.0f);
                        tmPresentation.vertexSnapStep = std::clamp(tmPresentation.vertexSnapStep, 0.0005f, 4.0f);
                        tmPresentation.screenSnapStep = std::clamp(tmPresentation.screenSnapStep, 0.25f, 16.0f);
                    }
                    return changed;
                });
            }
        }

        if (sectionVisible("Debug / Developer", ProjectSettingsVisibilityMode::Advanced) &&
            DrawSettingSection("[D]", Loc::T("SETTINGS_EDITOR_DEBUG_DEVELOPER", "Debug / Developer"), Loc::T("SETTINGS_EDITOR_DEBUG_DEVELOPER_DESC", "Debug overlays and internal controls."), false)) {
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Debug / Developer", "Scene Gizmos", "camera light gizmos")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_SCENE_GIZMOS", "Scene Gizmos"), Loc::T("SETTINGS_EDITOR_SCENE_GIZMOS_DESC", "Shows editor-only visual handles and overlays."), [&]() {
                    return ImGui::Checkbox("##SceneGizmos", &showSceneGizmos);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Debug / Developer", "3D Grid", "scene grid")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_3_D_GRID", "3D Grid"), Loc::T("SETTINGS_EDITOR_3_D_GRID_DESC", "Shows the scene grid in 3D viewports."), [&]() {
                    return ImGui::Checkbox("##SceneGrid3D", &showSceneGrid3D);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Debug / Developer", "Show Developer Tools", "reveal debug sections menus")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_SHOW_DEVELOPER_TOOLS", "Show Developer Tools"), Loc::T("SETTINGS_EDITOR_SHOW_DEVELOPER_TOOLS_DESC", "Reveals debug sections and extra developer menus."), [&]() {
                    return ImGui::Checkbox("##ShowDeveloperTools", &revealDebugSectionsAndMenus);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Debug / Developer", "Selection Collider Bounds", "collision wireframe physics")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_SELECTION_COLLIDER_BOUNDS", "Selection Collider Bounds"), Loc::T("SETTINGS_EDITOR_SELECTION_COLLIDER_BOUNDS_DESC", "Draws collider wireframes for selected objects."), [&]() {
                    return ImGui::Checkbox("##SelectionColliderBounds", &collisionWireframe);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Debug / Developer", "Editor VSync", "performance frame rate presentation tearing stutter")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_EDITOR_VSYNC", "Editor VSync"), Loc::T("SETTINGS_EDITOR_EDITOR_VSYNC_DESC", "Paces the editor window to the display refresh. Turning this off lets the editor free-run, which makes frame times swing as the driver throttles presentation."), [&]() {
                    if (ImGui::Checkbox("##EditorVSyncEnabled", &editorVSyncEnabled)) {
                        applyPresentationSettings();
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Debug / Developer", "FPS Cap", "performance frame rate limit")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_FPS_CAP", "FPS Cap"), Loc::T("SETTINGS_EDITOR_FPS_CAP_DESC", "Limits frame rate when VSync is off. It is bypassed while presentation is already synchronized."), [&]() {
                    bool changed = ImGui::Checkbox("Enabled##FpsCapEnabled", &fpsCapEnabled);
                    ImGui::BeginDisabled(!fpsCapEnabled);
                    changed |= ImGui::DragFloat("Target##FpsCapTarget", &fpsCap, 1.0f, 1.0f, 500.0f, "%.0f");
                    ImGui::EndDisabled();
                    if (changed) fpsCap = std::max(1.0f, fpsCap);
                    return changed;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Debug / Developer", "2D Light Debug", "stats buffer scale lighting resolution")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_EDITOR_2_D_LIGHT_DEBUG", "2D Light Debug"), Loc::T("SETTINGS_EDITOR_2_D_LIGHT_DEBUG_DESC", "Lighting stats overlay and internal render resolution (ships with the game)."), [&]() {
                    bool statsChanged = ImGui::Checkbox("Stats##Light2DStatsOverlay", &showLight2DStatsOverlay);
                    bool resAuto = ImGui::Checkbox("Auto##Light2DResAutoDbg", &buildSettings.light2DLightingResolutionAuto);
                    ImGui::SameLine();
                    bool resSlider = ImGui::SliderFloat("Res##Light2DResDbg", &buildSettings.light2DLightingResolution, 0.25f, 1.0f, "%.2fx");
                    if (resSlider) buildSettings.light2DLightingResolution = std::clamp(buildSettings.light2DLightingResolution, 0.25f, 1.0f);
                    if (resAuto || resSlider) buildSettingsChanged = true;
                    return statsChanged;
                });
            }
            if (revealDebugSectionsAndMenus &&
                visible(ProjectSettingsVisibilityMode::Developer, "Debug / Developer", "Game Profiler", "internal performance details")) {
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Game Profiler", ImGuiTreeNodeFlags_DefaultOpen)) {
                    drawGameProfilerContent();
                }
            }
        }

        if (settingsSearch[0] != '\0' && visibleSettingCount == 0) {
            ImGui::TextDisabled("No settings match this search.");
        }

        if (editorSettingsChanged) {
            audioPreviewVolume = std::clamp(audioPreviewVolume, 0.0f, 2.0f);
            saveEditorUserSettings();
        }
        if (buildSettingsChanged) {
            saveBuildSettings();
        }
    } else if (selectedTab == kBuildTab) {
        bool changed = false;

        if (sectionVisible("Build Targets", ProjectSettingsVisibilityMode::Simple) &&
            DrawSettingSection("[B]", Loc::T("SETTINGS_BUILD_BUILD_TARGETS", "Build Targets"), Loc::T("SETTINGS_BUILD_BUILD_TARGETS_DESC", "Platform and packaging options for exported builds."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Build Targets", "Platform", "windows linux android")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_BUILD_PLATFORM", "Platform"), Loc::T("SETTINGS_BUILD_PLATFORM_DESC", "Target operating system for the build profile."), [&]() {
                    int targetIndex = buildPlatformIndex(buildSettings.platform);
                    bool rowChanged = false;
                    if (ImGui::Combo("##BuildPlatform", &targetIndex,
                                     kBuildPlatformLabels, kBuildPlatformCount)) {
                        BuildPlatform newPlatform = buildPlatformFromIndex(targetIndex);
                        if (newPlatform != buildSettings.platform) {
                            buildSettings.platform = newPlatform;
                            int archCount = 0;
                            const BuildArchitectureInfo* archList = buildArchitecturesForPlatform(newPlatform, archCount);
                            bool valid = false;
                            for (int i = 0; i < archCount; ++i) {
                                if (buildSettings.architecture == archList[i].serializedName) { valid = true; break; }
                            }
                            if (!valid) buildSettings.architecture = archList[0].serializedName;
                            rowChanged = true;
                        }
                    }
                    if (buildPlatformIsExperimental(buildSettings.platform)) {
                        ImGui::TextDisabled("Experimental — runtime is not fully wired up yet.");
                    }
                    return rowChanged;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Build Targets", "Architecture", "x86 x86_64 arm arm64 cpu")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_BUILD_ARCHITECTURE", "Architecture"), Loc::T("SETTINGS_BUILD_ARCHITECTURE_DESC", "CPU architecture for the target platform."), [&]() {
                    int archCount = 0;
                    const BuildArchitectureInfo* archList = buildArchitecturesForPlatform(buildSettings.platform, archCount);
                    const char* archLabels[8];
                    int archLabelsUsed = std::min(archCount, static_cast<int>(sizeof(archLabels) / sizeof(archLabels[0])));
                    int archIndex = 0;
                    for (int i = 0; i < archLabelsUsed; ++i) {
                        archLabels[i] = archList[i].label;
                        if (buildSettings.architecture == archList[i].serializedName) archIndex = i;
                    }
                    if (ImGui::Combo("##BuildArchitecture", &archIndex, archLabels, archLabelsUsed)) {
                        buildSettings.architecture = archList[archIndex].serializedName;
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Build Targets", "Development Build", "debug development")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_BUILD_DEVELOPMENT_BUILD", "Development Build"), Loc::T("SETTINGS_BUILD_DEVELOPMENT_BUILD_DESC", "Build with development options enabled."), [&]() {
                    return ImGui::Checkbox("##BuildDevelopment", &buildSettings.developmentBuild);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Build Targets", "Debugging", "script debugging profiler deep profiling")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_BUILD_DEBUGGING", "Debugging"), Loc::T("SETTINGS_BUILD_DEBUGGING_DESC", "Developer diagnostics for exported builds."), [&]() {
                    bool rowChanged = false;
                    rowChanged |= ImGui::Checkbox("Script##BuildScriptDebugging", &buildSettings.scriptDebugging);
                    ImGui::SameLine();
                    rowChanged |= ImGui::Checkbox("Profiler##BuildAutoProfiler", &buildSettings.autoConnectProfiler);
                    ImGui::SameLine();
                    rowChanged |= ImGui::Checkbox("Deep##BuildDeepProfiling", &buildSettings.deepProfiling);
                    return rowChanged;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Build Targets", "Special Builds", "scripts only server")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_BUILD_SPECIAL_BUILDS", "Special Builds"), Loc::T("SETTINGS_BUILD_SPECIAL_BUILDS_DESC", "Internal build modes for script-only or server exports."), [&]() {
                    bool rowChanged = false;
                    rowChanged |= ImGui::Checkbox("Scripts Only##BuildScriptsOnly", &buildSettings.scriptsOnlyBuild);
                    ImGui::SameLine();
                    rowChanged |= ImGui::Checkbox("Server##BuildServer", &buildSettings.serverBuild);
                    return rowChanged;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Build Targets", "Compression", "package archive lz4")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_BUILD_COMPRESSION", "Compression"), Loc::T("SETTINGS_BUILD_COMPRESSION_DESC", "Compression method used by exported packages."), [&]() {
                    const char* compressionOptions[] = {"Default", "None", "LZ4", "LZ4HC"};
                    int compressionIndex = 0;
                    for (int i = 0; i < IM_ARRAYSIZE(compressionOptions); ++i) {
                        if (buildSettings.compressionMethod == compressionOptions[i]) {
                            compressionIndex = i;
                            break;
                        }
                    }
                    if (ImGui::Combo("##BuildCompression", &compressionIndex, compressionOptions, IM_ARRAYSIZE(compressionOptions))) {
                        buildSettings.compressionMethod = compressionOptions[compressionIndex];
                        return true;
                    }
                    return false;
                });
            }
        }

        if (sectionVisible("Scenes In Build", ProjectSettingsVisibilityMode::Simple) &&
            DrawSettingSection("[S]", Loc::T("SETTINGS_BUILD_SCENES_IN_BUILD", "Scenes In Build"), Loc::T("SETTINGS_BUILD_SCENES_IN_BUILD_DESC", "Scene list included in the exported build."))) {
            ImGui::BeginChild("ProjectBuildScenes", ImVec2(0, 180), true);
            for (int i = 0; i < static_cast<int>(buildSettings.scenes.size()); ++i) {
                BuildSceneEntry& entry = buildSettings.scenes[i];
                if (!ProjectSettingsMatchesSearch(settingsSearch, "Scenes In Build", entry.name.c_str(), "scene build")) {
                    continue;
                }
                ++visibleSettingCount;
                ImGui::PushID(i);
                bool enabled = entry.enabled;
                if (ImGui::Checkbox("##enabled", &enabled)) {
                    entry.enabled = enabled;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Selectable(entry.name.c_str(), buildSettingsSelectedIndex == i, ImGuiSelectableFlags_SpanAllColumns)) {
                    buildSettingsSelectedIndex = i;
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            if (ImGui::Button("Add Current Scene")) {
                if (addSceneToBuildSettings(projectManager.currentProject.currentSceneName, true)) {
                    changed = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Add All Scenes")) {
                auto scenes = projectManager.currentProject.getSceneList();
                for (const auto& scene : scenes) {
                    if (addSceneToBuildSettings(scene, scene == projectManager.currentProject.currentSceneName)) {
                        changed = true;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove Selected")) {
                if (buildSettingsSelectedIndex >= 0 &&
                    buildSettingsSelectedIndex < static_cast<int>(buildSettings.scenes.size())) {
                    buildSettings.scenes.erase(buildSettings.scenes.begin() + buildSettingsSelectedIndex);
                    if (buildSettingsSelectedIndex >= static_cast<int>(buildSettings.scenes.size())) {
                        buildSettingsSelectedIndex = static_cast<int>(buildSettings.scenes.size()) - 1;
                    }
                    changed = true;
                }
            }
        }

        if (sectionVisible("Export", ProjectSettingsVisibilityMode::Simple) &&
            DrawSettingSection("[E]", Loc::T("SETTINGS_BUILD_EXPORT", "Export"), Loc::T("SETTINGS_BUILD_EXPORT_DESC", "Save or open the dedicated build/export window."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Export", "Build Window", "advanced export")) {
                DrawSettingRow(Loc::T("SETTINGS_BUILD_BUILD_WINDOW", "Build Window"), Loc::T("SETTINGS_BUILD_BUILD_WINDOW_DESC", "Open the full build/export window."), [&]() {
                    if (ImGui::Button("Open Advanced Build Window")) {
                        showBuildSettings = true;
                        return true;
                    }
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "Export", "Save Build Profile", "save profile")) {
                DrawSettingRow(Loc::T("SETTINGS_BUILD_SAVE_PROFILE", "Save Profile"), Loc::T("SETTINGS_BUILD_SAVE_PROFILE_DESC", "Save the current build profile."), [&]() {
                    if (ImGui::Button("Save Build Profile")) {
                        saveBuildSettings();
                        return true;
                    }
                    return false;
                });
            }
        }

        if (settingsSearch[0] != '\0' && visibleSettingCount == 0) {
            ImGui::TextDisabled("No build settings match this search.");
        }

        if (changed) {
            saveBuildSettings();
        }
    } else if (selectedTab == kCompilationTab) {
        struct CompilationUiState {
            fs::path configPath;
            ScriptBuildConfig config;
            bool loaded = false;
            bool dirty = false;
            std::string status;
        };
        static CompilationUiState ui;

        fs::path configPath = resolveScriptsConfigPath(projectManager.currentProject);
        if (!ui.loaded || ui.configPath != configPath) {
            ui = CompilationUiState{};
            ui.configPath = configPath;
            std::string error;
            if (!scriptCompiler.loadConfig(configPath, ui.config, error)) {
                ui.status = "Creating new scripts config (previous load failed): " + error;
                ui.config = ScriptBuildConfig{};
                ui.config.scriptsDir = projectManager.currentProject.projectPath / "Assets" / "Scripts";
                ui.config.outDir = projectManager.currentProject.projectPath / "Library" / "CompiledScripts";
                ui.loaded = true;
            } else {
                ui.status = "Loaded " + configPath.filename().string();
                ui.loaded = true;
            }
        }

        bool editorSettingsChanged = false;
        if (sectionVisible("Compiler Workflow", ProjectSettingsVisibilityMode::Simple) &&
            DrawSettingSection("[C]", Loc::T("SETTINGS_COMPILATION_COMPILER_WORKFLOW", "Compiler Workflow"), Loc::T("SETTINGS_COMPILATION_COMPILER_WORKFLOW_DESC", "C++ script compiler workflow settings."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Compiler Workflow", "Auto-compile on Save", "scripts compile")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_COMPILATION_AUTO_COMPILE_ON_SAVE", "Auto-compile on Save"), Loc::T("SETTINGS_COMPILATION_AUTO_COMPILE_ON_SAVE_DESC", "Compile scripts automatically when a script file is saved."), [&]() {
                    return ImGui::Checkbox("##ScriptAutoCompileOnSave", &scriptEditorState.autoCompileOnSave);
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Compiler Workflow", "Scan Interval", "auto compile scan seconds")) {
                editorSettingsChanged |= DrawSettingRow(Loc::T("SETTINGS_COMPILATION_SCAN_INTERVAL", "Scan Interval"), Loc::T("SETTINGS_COMPILATION_SCAN_INTERVAL_DESC", "How often the editor checks for script changes."), [&]() {
                    float interval = static_cast<float>(scriptAutoCompileInterval);
                    if (ImGui::SliderFloat("##ScriptAutoCompileInterval", &interval, 0.1f, 10.0f, "%.2f s")) {
                        scriptAutoCompileInterval = std::clamp(static_cast<double>(interval), 0.1, 10.0);
                        return true;
                    }
                    return false;
                });
            }
        }

        if (sectionVisible("scripts.modu", ProjectSettingsVisibilityMode::Simple) &&
            drawSettingSectionIcon("Resources/Engine-Root/Project Settings/Dropdowns/Editor/ModuCPP Logo.png",
                                   "[S]", Loc::T("SETTINGS_COMPILATION_SCRIPTS_MODU", "scripts.modu"), Loc::T("SETTINGS_COMPILATION_SCRIPTS_MODU_DESC", "Project script compiler configuration and compatibility settings."))) {
            auto editString = [&](const char* label, std::string& value, size_t capacity = 1024) {
                std::vector<char> buf(capacity, '\0');
                std::snprintf(buf.data(), buf.size(), "%s", value.c_str());
                if (ImGui::InputText(label, buf.data(), buf.size())) {
                    value = buf.data();
                    ui.dirty = true;
                }
            };
            auto editPath = [&](const char* label, fs::path& value, size_t capacity = 1024) {
                std::string text = value.generic_string();
                std::vector<char> buf(capacity, '\0');
                std::snprintf(buf.data(), buf.size(), "%s", text.c_str());
                if (ImGui::InputText(label, buf.data(), buf.size())) {
                    value = fs::path(buf.data());
                    ui.dirty = true;
                }
            };

            auto canonicalCppStd = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (value == "c++2b") return std::string("c++23");
                if (value == "gnu++2b") return std::string("gnu++23");
                if (value == "c++2c") return std::string("c++26");
                if (value == "gnu++2c") return std::string("gnu++26");
                return value;
            };
            static const char* cppStdLabels[] = {
                "c++14",
                "c++17",
                "c++20",
                "c++23 (Default)",
                "c++26",
                "gnu++14",
                "gnu++17",
                "gnu++20",
                "gnu++23",
                "gnu++26"
            };
            static const char* cppStdValues[] = {
                "c++14",
                "c++17",
                "c++20",
                "c++23",
                "c++26",
                "gnu++14",
                "gnu++17",
                "gnu++20",
                "gnu++23",
                "gnu++26"
            };

            int cppIdx = 3;
            const std::string currentCppStandard = canonicalCppStd(ui.config.cppStandard);
            for (int i = 0; i < IM_ARRAYSIZE(cppStdValues); ++i) {
                if (currentCppStandard == cppStdValues[i]) {
                    cppIdx = i;
                    break;
                }
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "scripts.modu", "C++ Standard", "cpp c++26 c++23 c++20 c++17 c++14")) {
                DrawSettingRow(Loc::T("SETTINGS_COMPILATION_C_STANDARD", "C++ Standard"),
                               Loc::T("SETTINGS_COMPILATION_C_STANDARD_DESC", "Default is C++23. Use another standard only when a project needs a different target."),
                               [&]() {
                    if (ImGui::Combo("##CppStandard", &cppIdx, cppStdLabels, IM_ARRAYSIZE(cppStdLabels))) {
                        ui.config.cppStandard = cppStdValues[cppIdx];
                        ui.dirty = true;
                        return true;
                    }
                    return false;
                });
            }

            if (visible(ProjectSettingsVisibilityMode::Simple, "scripts.modu", "Scripts Directory", "source folder path")) {
                DrawSettingRow(Loc::T("SETTINGS_COMPILATION_SCRIPTS_DIRECTORY", "Scripts Directory"), Loc::T("SETTINGS_COMPILATION_SCRIPTS_DIRECTORY_DESC", "Folder containing project script source files."), [&]() {
                    editPath("##ScriptsDirectory", ui.config.scriptsDir);
                    return false;
                });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "scripts.modu", "Output Directory", "compiled scripts output folder")) {
                DrawSettingRow(Loc::T("SETTINGS_COMPILATION_OUTPUT_DIRECTORY", "Output Directory"), Loc::T("SETTINGS_COMPILATION_OUTPUT_DIRECTORY_DESC", "Folder where compiled script binaries are written."), [&]() {
                    editPath("##ScriptsOutputDirectory", ui.config.outDir);
                    return false;
                });
            }

            if (visible(ProjectSettingsVisibilityMode::Advanced, "scripts.modu", "Include Directories", "include path headers")) {
                ImGui::SeparatorText("Include Directories");
                for (size_t i = 0; i < ui.config.includeDirs.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    editPath("##inc", ui.config.includeDirs[i], 1200);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        ui.config.includeDirs.erase(ui.config.includeDirs.begin() + static_cast<long>(i));
                        ui.dirty = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("Add Include Directory")) {
                    ui.config.includeDirs.emplace_back(fs::path());
                    ui.dirty = true;
                }
            }

            if (visible(ProjectSettingsVisibilityMode::Advanced, "scripts.modu", "Defines", "preprocessor symbols")) {
                ImGui::SeparatorText("Defines");
                for (size_t i = 0; i < ui.config.defines.size(); ++i) {
                    ImGui::PushID(static_cast<int>(1000 + i));
                    editString("##def", ui.config.defines[i], 1024);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        ui.config.defines.erase(ui.config.defines.begin() + static_cast<long>(i));
                        ui.dirty = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("Add Define")) {
                    ui.config.defines.push_back("");
                    ui.dirty = true;
                }
            }

            if (visible(ProjectSettingsVisibilityMode::Developer, "scripts.modu", "Linux Link Libraries", "linker libraries linux")) {
                ImGui::SeparatorText("Linux Link Libraries");
                for (size_t i = 0; i < ui.config.linuxLinkLibs.size(); ++i) {
                    ImGui::PushID(static_cast<int>(2000 + i));
                    editString("##linlib", ui.config.linuxLinkLibs[i], 512);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        ui.config.linuxLinkLibs.erase(ui.config.linuxLinkLibs.begin() + static_cast<long>(i));
                        ui.dirty = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("Add Linux Lib")) {
                    ui.config.linuxLinkLibs.push_back("");
                    ui.dirty = true;
                }
            }

            if (visible(ProjectSettingsVisibilityMode::Developer, "scripts.modu", "Windows Link Libraries", "linker libraries windows")) {
                ImGui::SeparatorText("Windows Link Libraries");
                for (size_t i = 0; i < ui.config.windowsLinkLibs.size(); ++i) {
                    ImGui::PushID(static_cast<int>(3000 + i));
                    editString("##winlib", ui.config.windowsLinkLibs[i], 512);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        ui.config.windowsLinkLibs.erase(ui.config.windowsLinkLibs.begin() + static_cast<long>(i));
                        ui.dirty = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("Add Windows Lib")) {
                    ui.config.windowsLinkLibs.push_back("");
                    ui.dirty = true;
                }
            }

            if (ImGui::Button("Reload scripts.modu")) {
                ui.loaded = false;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!ui.dirty);
            if (ImGui::Button("Save scripts.modu")) {
                std::string error;
                if (SaveScriptsConfig(ui.configPath, ui.config, projectManager.currentProject.projectPath, error)) {
                    ui.status = "Saved " + ui.configPath.filename().string();
                    ui.dirty = false;
                    scriptingFilesDirty = true;
                    scriptLastAutoCompileTime.clear();
                    scriptAutoCompileCheckedSourceTime.clear();
                    scriptAutoCompileBinaryCache.clear();
                    scriptAutoCompileDiscoveredSources.clear();
                    ++scriptAutoCompileScanGeneration;
                    scriptAutoCompileConfigValid = false;
                    autoCompileQueue.clear();
                    autoCompileQueued.clear();
                    scriptAutoCompileLastDirectoryScan = 0.0;
                    managedAutoCompileLastScan = 0.0;
                    managedAutoCompileCachedProjectDir.clear();
                    managedAutoCompileNewestSource = fs::file_time_type{};
                    managedAutoCompileHasSource = false;
                    addConsoleMessage("Saved scripts config: " + ui.configPath.string(), ConsoleMessageType::Success);
                } else {
                    ui.status = error;
                    addConsoleMessage(error, ConsoleMessageType::Error);
                }
            }
            ImGui::EndDisabled();
            if (!ui.status.empty()) {
                ImGui::TextDisabled("%s", ui.status.c_str());
            }
        }

        if (settingsSearch[0] != '\0' && visibleSettingCount == 0) {
            ImGui::TextDisabled("No compilation settings match this search.");
        }

        if (editorSettingsChanged) {
            saveEditorUserSettings();
        }
    } else if (selectedTab == kLanguageManagerTab) {
        ProjectLanguageSettings& languageConfig = projectManager.currentProject.languageSettings;
        const std::vector<ModuCPPLang::LanguagePack>& languagePacks = ModuCPPLang::Languages();
        const std::string activeModuCppLanguageId = projectManager.effectiveModuCppLanguage();
        const ModuCPPLang::LanguagePack& activePack =
            ModuCPPLang::FindLanguageOrCanonical(activeModuCppLanguageId);
        const bool projectLoaded = projectManager.currentProject.isLoaded;
        bool changed = false;
        bool launcherChanged = false;

        static std::string languageTranslateStatus;
        static bool triggerTranslateProjectPopup = false;

        auto packLabels = [&]() {
            std::vector<const char*> labels;
            labels.reserve(languagePacks.size());
            for (const ModuCPPLang::LanguagePack& pack : languagePacks) {
                labels.push_back(pack.displayName.c_str());
            }
            return labels;
        };

        if (sectionVisible("Editor Language", ProjectSettingsVisibilityMode::Simple) &&
            drawSettingSectionIcon("Resources/Engine-Root/Project Settings/Tabs/Language Manager.png",
                                   "[L]", Loc::T("LANGUAGE_MANAGER_EDITOR_TITLE", "Editor Language"),
                                   Loc::T("LANGUAGE_MANAGER_EDITOR_DESC",
                                          "The language the Modularity editor itself is displayed in. This is a global editor preference and is not written into the project."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Editor Language",
                        "Editor Language", "editor ui language german english localization interface")) {
                launcherChanged |= DrawSettingRow(
                    Loc::T("LANGUAGE_MANAGER_EDITOR_LANGUAGE", "Editor Language"),
                    Loc::T("LANGUAGE_MANAGER_EDITOR_LANGUAGE_DESC",
                           "Applies immediately to open windows. Stored per user, never in project files."),
                    [&]() {
                        const std::vector<Modularity::Loc::LanguageInfo>& uiLanguages = Loc::Languages();
                        std::vector<const char*> labels;
                        labels.reserve(uiLanguages.size());
                        for (const Modularity::Loc::LanguageInfo& info : uiLanguages) {
                            labels.push_back(info.displayName.c_str());
                        }
                        int index = Loc::LanguageIndex(projectManager.editorLanguage);
                        if (ImGui::Combo("##EditorLanguage", &index, labels.data(),
                                         static_cast<int>(labels.size()))) {
                            index = std::clamp(index, 0, static_cast<int>(uiLanguages.size()) - 1);
                            projectManager.editorLanguage = uiLanguages[static_cast<size_t>(index)].id;
                            Loc::SetLanguage(projectManager.editorLanguage);
                            return true;
                        }
                        return false;
                    });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Editor Language",
                        "Language Files", "reload translation json localization files")) {
                DrawSettingRow(Loc::T("LANGUAGE_MANAGER_EDITOR_RELOAD", "Language Files"),
                               Loc::T("LANGUAGE_MANAGER_EDITOR_RELOAD_DESC",
                                      "Re-reads Resources/Languages from disk. Useful while writing a translation."),
                               [&]() {
                    if (ImGui::Button(Loc::T("LANGUAGE_MANAGER_EDITOR_RELOAD_BUTTON", "Reload Language Files"))) {
                        pendingLanguageFileReload = true;
                    }
                    return false;
                });
                ImGui::TextDisabled("Resources/Languages/<Language>/Editor.json, ModuCPP.json");
            }
            if (visible(ProjectSettingsVisibilityMode::Developer, "Editor Language",
                        "Missing Keys", "localization missing untranslated keys diagnostics")) {
                ImGui::TextColored(ImVec4(0.86f, 0.90f, 0.96f, 1.0f), "%s",
                                   Loc::T("LANGUAGE_MANAGER_EDITOR_MISSING", "Missing Keys"));
                DrawHelpTooltip(Loc::T("LANGUAGE_MANAGER_EDITOR_MISSING_DESC",
                                       "Keys requested this session that no language defined. They render as the key itself."));
                const std::vector<std::string>& missing = Loc::MissingKeys();
                if (missing.empty()) {
                    ImGui::TextDisabled("%s", Loc::T("LANGUAGE_MANAGER_EDITOR_MISSING_NONE",
                                                     "No missing keys this session."));
                } else {
                    for (const std::string& key : missing) {
                        ImGui::BulletText("%s", key.c_str());
                    }
                }
                ImGui::Spacing();
            }
        }

        if (sectionVisible("ModuCPP Code Language", ProjectSettingsVisibilityMode::Simple) &&
            DrawSettingSection("[M]", Loc::T("LANGUAGE_MANAGER_MODUCPP_TITLE", "ModuCPP Code Language"),
                               Loc::T("LANGUAGE_MANAGER_MODUCPP_DESC",
                                      "The localized ModuCPP syntax this project's scripts are written in. Independent of the editor language."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "ModuCPP Code Language",
                        "Use Global Default", "global default project override moducpp language")) {
                changed |= DrawSettingRow(
                    Loc::T("LANGUAGE_MANAGER_MODUCPP_USE_GLOBAL_DEFAULT", "Use Global Default"),
                    Loc::T("LANGUAGE_MANAGER_MODUCPP_USE_GLOBAL_DEFAULT_DESC",
                           "Follow the global ModuCPP code language instead of storing a per-project choice."),
                    [&]() {
                        bool useGlobal = languageConfig.usesGlobalModuCppDefault();
                        ImGui::BeginDisabled(!projectLoaded);
                        const bool toggled = ImGui::Checkbox("##ModuCppUseGlobalDefault", &useGlobal);
                        ImGui::EndDisabled();
                        if (toggled) {
                            languageConfig.moduCppSyntaxLanguage =
                                useGlobal ? std::string()
                                          : ModuCPPLang::NormalizeLanguageId(projectManager.moduCppDefaultLanguage);
                            languageTranslateStatus.clear();
                            return true;
                        }
                        return false;
                    });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "ModuCPP Code Language",
                        "Code Language", "moducpp language german english localization keywords syntax")) {
                changed |= DrawSettingRow(
                    Loc::T("LANGUAGE_MANAGER_MODUCPP_LANGUAGE", "Code Language"),
                    Loc::T("LANGUAGE_MANAGER_MODUCPP_LANGUAGE_DESC",
                           "Keywords only. Classes, fields, methods, namespaces and assets are never renamed."),
                    [&]() {
                        const bool usesGlobal = languageConfig.usesGlobalModuCppDefault();
                        std::vector<const char*> labels = packLabels();
                        int index = ModuCPPLang::LanguageIndex(projectManager.effectiveModuCppLanguage());
                        ImGui::BeginDisabled(usesGlobal || !projectLoaded);
                        const bool picked = ImGui::Combo("##ModuCppSyntaxLanguage", &index, labels.data(),
                                                         static_cast<int>(labels.size()));
                        ImGui::EndDisabled();
                        if (picked) {
                            index = std::clamp(index, 0, static_cast<int>(languagePacks.size()) - 1);
                            languageConfig.moduCppSyntaxLanguage = languagePacks[static_cast<size_t>(index)].id;
                            languageTranslateStatus.clear();
                            return true;
                        }
                        return false;
                    });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "ModuCPP Code Language",
                        "Global Default", "global default new projects moducpp language")) {
                launcherChanged |= DrawSettingRow(
                    Loc::T("LANGUAGE_MANAGER_MODUCPP_GLOBAL_DEFAULT", "Global Default"),
                    Loc::T("LANGUAGE_MANAGER_MODUCPP_GLOBAL_DEFAULT_DESC",
                           "The ModuCPP code language new projects start with, and the fallback for projects that do not override it."),
                    [&]() {
                        std::vector<const char*> labels = packLabels();
                        int index = ModuCPPLang::LanguageIndex(projectManager.moduCppDefaultLanguage);
                        if (ImGui::Combo("##ModuCppGlobalDefaultLanguage", &index, labels.data(),
                                         static_cast<int>(labels.size()))) {
                            index = std::clamp(index, 0, static_cast<int>(languagePacks.size()) - 1);
                            projectManager.moduCppDefaultLanguage = languagePacks[static_cast<size_t>(index)].id;
                            languageTranslateStatus.clear();
                            return true;
                        }
                        return false;
                    });
            }

            if (visible(ProjectSettingsVisibilityMode::Simple, "ModuCPP Code Language",
                        "Translate Scripts", "convert translate project scripts language")) {
                DrawSettingRow(Loc::T("LANGUAGE_MANAGER_MODUCPP_TRANSLATE", "Translate Scripts"),
                               Loc::T("LANGUAGE_MANAGER_MODUCPP_TRANSLATE_DESC",
                                      "Rewrites every .moducpp file under Assets/ into the selected syntax language."),
                               [&]() {
                    char buttonLabel[192];
                    std::snprintf(buttonLabel, sizeof(buttonLabel),
                                  Loc::T("LANGUAGE_MANAGER_MODUCPP_TRANSLATE_BUTTON", "Translate to %s..."),
                                  activePack.endonym.c_str());
                    ImGui::BeginDisabled(!projectLoaded);
                    if (ImGui::Button(buttonLabel)) {
                        triggerTranslateProjectPopup = true;
                    }
                    ImGui::EndDisabled();
                    return false;
                });
                if (!languageTranslateStatus.empty()) {
                    ImGui::TextDisabled("%s", languageTranslateStatus.c_str());
                }
            }

            if (visible(ProjectSettingsVisibilityMode::Advanced, "ModuCPP Code Language",
                        "Keyword Dictionary", "keywords mapping table dictionary")) {
                ImGui::TextColored(ImVec4(0.86f, 0.90f, 0.96f, 1.0f), "%s",
                                   Loc::T("LANGUAGE_MANAGER_MODUCPP_DICTIONARY", "Keyword Dictionary"));
                DrawHelpTooltip(Loc::T("LANGUAGE_MANAGER_MODUCPP_DICTIONARY_DESC",
                                       "The dictionary this language compiles through. It is the only part of the compiler that changes between languages."));
                if (activePack.aliases.empty()) {
                    ImGui::TextDisabled("%s", Loc::T("LANGUAGE_MANAGER_MODUCPP_DICTIONARY_NONE",
                                                     "English is the canonical ModuCPP syntax, so it has no aliases."));
                } else if (ImGui::BeginTable("##ModuCppKeywordDictionary", 3,
                                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings,
                                             ImVec2(0.0f, 200.0f))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn(activePack.endonym.c_str());
                    ImGui::TableSetupColumn(Loc::T("LANGUAGE_MANAGER_MODUCPP_COL_MODU_CPP", "ModuCPP"));
                    ImGui::TableSetupColumn(Loc::T("LANGUAGE_MANAGER_MODUCPP_COL_WHERE", "Where"));
                    ImGui::TableHeadersRow();
                    for (const ModuCPPLang::KeywordAlias& alias : activePack.aliases) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(alias.localized.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(alias.canonical.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextDisabled("%s", alias.position == ModuCPPLang::AliasPosition::Member
                                                      ? Loc::T("LANGUAGE_MANAGER_MODUCPP_WHERE_MEMBER", "built-in API")
                                                      : Loc::T("LANGUAGE_MANAGER_MODUCPP_WHERE_KEYWORD", "keyword"));
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
            }
        }

        if (sectionVisible("Import Translation", ProjectSettingsVisibilityMode::Simple) &&
            DrawSettingSection("[I]", Loc::T("LANGUAGE_MANAGER_IMPORT_TITLE", "Import Translation"),
                               Loc::T("LANGUAGE_MANAGER_IMPORT_DESC",
                                      "What to do when a script written in another supported language is imported."))) {
            if (visible(ProjectSettingsVisibilityMode::Simple, "Import Translation",
                        "On Import", "import translate ask popup dont ask again")) {
                changed |= DrawSettingRow(
                    Loc::T("LANGUAGE_MANAGER_IMPORT_ON_IMPORT", "On Import"),
                    Loc::T("LANGUAGE_MANAGER_IMPORT_ON_IMPORT_DESC",
                           "Detected automatically from the imported source. 'Ask every time' shows the import popup."),
                    [&]() {
                    int policy = static_cast<int>(languageConfig.importTranslatePolicy);
                    const char* policies[] = {
                        Loc::T("LANGUAGE_MANAGER_IMPORT_ASK", "Ask every time"),
                        Loc::T("LANGUAGE_MANAGER_IMPORT_ALWAYS", "Translate to this project's language"),
                        Loc::T("LANGUAGE_MANAGER_IMPORT_NEVER", "Keep the original language")
                    };
                    ImGui::BeginDisabled(!projectLoaded);
                    const bool picked = ImGui::Combo("##ModuCppImportTranslatePolicy", &policy, policies,
                                                     IM_ARRAYSIZE(policies));
                    ImGui::EndDisabled();
                    if (picked) {
                        languageConfig.importTranslatePolicy =
                            static_cast<ProjectImportTranslatePolicy>(std::clamp(policy, 0, 2));
                        return true;
                    }
                    return false;
                });
            }
            if (languageConfig.importTranslatePolicy != ProjectImportTranslatePolicy::Ask &&
                visible(ProjectSettingsVisibilityMode::Simple, "Import Translation",
                        "Ask Again", "reset dont ask again import popup")) {
                changed |= DrawSettingRow(
                    Loc::T("LANGUAGE_MANAGER_IMPORT_ASK_AGAIN", "Ask Again"),
                    Loc::T("LANGUAGE_MANAGER_IMPORT_ASK_AGAIN_DESC",
                           "Re-enables the import popup that was dismissed with 'Don't ask again'."),
                    [&]() {
                    if (ImGui::Button(Loc::T("LANGUAGE_MANAGER_IMPORT_ASK_AGAIN_BUTTON",
                                             "Re-enable Import Prompt"))) {
                        languageConfig.importTranslatePolicy = ProjectImportTranslatePolicy::Ask;
                        return true;
                    }
                    return false;
                });
            }
        }

        if (settingsSearch[0] != '\0' && visibleSettingCount == 0) {
            ImGui::TextDisabled("No language settings match this search.");
        }

        if (triggerTranslateProjectPopup) {
            ImGui::OpenPopup(Loc::WindowRef("Translate Project Scripts"));
            triggerTranslateProjectPopup = false;
        }
        if (ImGui::BeginPopupModal(Loc::Window("DIALOG_TRANSLATE_PROJECT_TITLE", "Translate Project Scripts"),
                                   nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(Loc::T("DIALOG_TRANSLATE_PROJECT_BODY",
                                      "Rewrite every .moducpp script in this project into %s?"),
                               activePack.displayName.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("%s", Loc::T("DIALOG_TRANSLATE_PROJECT_NOTE1",
                                             "Only language keywords and built-in API aliases change."));
            ImGui::TextDisabled("%s", Loc::T("DIALOG_TRANSLATE_PROJECT_NOTE2",
                                             "Classes, fields, methods, namespaces and assets keep their names."));
            ImGui::TextDisabled("%s", Loc::T("DIALOG_TRANSLATE_PROJECT_NOTE3",
                                             "Behavior and generated C++ stay identical."));
            ImGui::Spacing();
            if (ImGui::Button(Loc::T("COMMON_TRANSLATE", "Translate"), ImVec2(140, 0))) {
                const fs::path scanRoot = projectManager.currentProject.assetsPath.empty()
                    ? projectManager.currentProject.projectPath
                    : projectManager.currentProject.assetsPath;
                const ModuCPPLang::ScriptLanguageSurvey survey =
                    ModuCPPLang::SurveyScripts(scanRoot, activePack);
                const ModuCPPLang::TranslationReport report =
                    ModuCPPLang::TranslateScriptFiles(survey.files, activePack);
                std::ostringstream summary;
                summary << "Translated " << report.translated << " script(s) to "
                        << activePack.displayName;
                if (report.failed > 0) summary << ", " << report.failed << " failed";
                languageTranslateStatus = summary.str();
                addConsoleMessage(languageTranslateStatus,
                                  report.failed > 0 ? ConsoleMessageType::Warning : ConsoleMessageType::Success);
                if (!report.firstError.empty()) {
                    addConsoleMessage(report.firstError, ConsoleMessageType::Error);
                }
                fileBrowser.needsRefresh = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(Loc::T("COMMON_CANCEL", "Cancel"), ImVec2(140, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (launcherChanged) projectManager.saveLauncherSettings();
        if (changed) projectManager.currentProject.saveProjectFile();
    } else if (selectedTab == kOpenXRTab) {
        namespace XR = Modularity::XR;
        XR::ProjectOpenXRSettings& xr = projectManager.currentProject.openXRSettings;
        const XR::XRDiagnosticsSnapshot& diag = XR::Diagnostics();
        bool changed = false;

        constexpr bool openXrCompiledIn = (MODULARITY_HAS_OPENXR != 0);

        const auto platformTarget = static_cast<XR::XRPlatformTarget>(openXRPlatformTabIndex);
        XR::XRTargetSettings& target = xr.target(platformTarget);
        const bool isPCTab = (platformTarget == XR::XRPlatformTarget::PC);

        const bool xrOn = xr.enabled;
        const bool targetOn = xrOn && target.enabled;

        if (DrawSettingSection("[XR]", Loc::T("SETTINGS_OPENXR_GENERAL", "General"),
                               Loc::T("SETTINGS_OPENXR_GENERAL_DESC",
                                      "Master switch and the settings that apply to every "
                                      "platform target."))) {
            if (!openXrCompiledIn) {
                DrawHelpText("This build was compiled with MODULARITY_ENABLE_OPENXR=OFF. "
                             "These settings still save with the project, but no OpenXR "
                             "runtime can be started until you rebuild with it ON.");
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "General", "Enable OpenXR",
                        "vr openxr quest headset pcvr steamvr")) {
                changed |= DrawSettingRow(Loc::T("SETTINGS_OPENXR_ENABLE", "Enable OpenXR"),
                                          Loc::T("SETTINGS_OPENXR_ENABLE_DESC",
                                                 "Run this project as a VR application. When off, "
                                                 "the engine never loads an OpenXR loader and every "
                                                 "platform behaves exactly as it does without VR."),
                                          [&]() { return ImGui::Checkbox("##OpenXREnabled", &xr.enabled); });
            }

            ImGui::BeginDisabled(!xrOn);
            if (visible(ProjectSettingsVisibilityMode::Simple, "General", "Target Support",
                        "platform target pc quest android build")) {
                changed |= DrawSettingRow(
                    (std::string(XR::DisplayName(platformTarget)) + " Support").c_str(),
                    XR::Description(platformTarget),
                    [&]() { return ImGui::Checkbox("##OpenXRTargetEnabled", &target.enabled); });
            }
            if (!XR::IsPlatformTargetSupported(platformTarget)) {
                DrawHelpText("This editor build has no graphics binding for this target, so it "
                             "cannot start a session here. The settings still save with the "
                             "project and apply when you build for it.");
            }

            ImGui::BeginDisabled(!targetOn);
            if (visible(ProjectSettingsVisibilityMode::Advanced, "General", "Graphics Binding",
                        "opengl es gles binding wgl egl")) {
                DrawSettingRow(Loc::T("SETTINGS_OPENXR_GRAPHICS_BACKEND", "Graphics Binding"),
                               Loc::T("SETTINGS_OPENXR_GRAPHICS_BACKEND_DESC",
                                      "The OpenXR graphics binding this target uses. It follows "
                                      "from the platform rather than being a choice: Android and "
                                      "Quest can only bind OpenGL ES, and a desktop runtime can "
                                      "only bind desktop OpenGL. A Vulkan XR binding is "
                                      "deliberately not offered rather than shipped half-working."),
                               [&]() {
                                   const char* backend =
                                       XR::ToString(XR::GraphicsBackendFor(platformTarget));
                                   ImGui::BeginDisabled(true);
                                   int index = 0;
                                   ImGui::Combo("##OpenXRGraphicsBackend", &index, &backend, 1);
                                   ImGui::EndDisabled();
                                   return false;
                               });
            }
            if (visible(ProjectSettingsVisibilityMode::Simple, "General", "Tracking Origin",
                        "floor stage eye local roomscale seated")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_OPENXR_TRACKING_ORIGIN", "Tracking Origin"),
                    Loc::T("SETTINGS_OPENXR_TRACKING_ORIGIN_DESC",
                           "Which OpenXR reference space the XR Origin maps onto. Floor is roomscale "
                           "with the origin on the floor (STAGE); Eye Level is seated, with the origin "
                           "at the recentre pose (LOCAL). Shared by every target: it describes the "
                           "application, not the hardware."),
                    [&]() {
                        int origin = (xr.trackingOrigin == XR::XRTrackingOrigin::Eye) ? 1 : 0;
                        const char* options[] = { "Floor", "Eye Level" };
                        if (ImGui::Combo("##OpenXRTrackingOrigin", &origin, options, IM_ARRAYSIZE(options))) {
                            xr.trackingOrigin = (origin == 1) ? XR::XRTrackingOrigin::Eye
                                                              : XR::XRTrackingOrigin::Floor;
                            return true;
                        }
                        return false;
                    });
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }

        if (DrawSettingSection("[R]", Loc::T("SETTINGS_OPENXR_RENDERING", "Rendering"),
                               Loc::T("SETTINGS_OPENXR_RENDERING_DESC",
                                      "How the two eye views are produced for this target."))) {
            ImGui::BeginDisabled(!targetOn);
            if (visible(ProjectSettingsVisibilityMode::Simple, "Rendering", "Render Mode",
                        "single pass multiview multi pass stereo")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_OPENXR_RENDER_MODE", "Render Mode"),
                    Loc::T("SETTINGS_OPENXR_RENDER_MODE_DESC",
                           "Single Pass draws both eyes in one pass into a 2-layer array texture "
                           "(needs GL_OVR_multiview2) and falls back to Multi Pass at runtime when "
                           "the extension is missing. Multi Pass draws one pass per eye and always works."),
                    [&]() {
                        int mode = (target.renderMode == XR::XRRenderMode::MultiPass) ? 1 : 0;
                        const char* modes[] = { "Single Pass / Multiview", "Multi Pass" };
                        if (ImGui::Combo("##OpenXRRenderMode", &mode, modes, IM_ARRAYSIZE(modes))) {
                            target.renderMode = (mode == 1) ? XR::XRRenderMode::MultiPass
                                                            : XR::XRRenderMode::SinglePassMultiview;
                            return true;
                        }
                        return false;
                    });
                if (target.renderMode == XR::XRRenderMode::SinglePassMultiview) {
                    if (diag.systemAcquired && !diag.multiviewSupported) {
                        DrawHelpText("This runtime did not report multiview support, so the session "
                                     "will run Multi Pass.");
                    } else if (isPCTab) {
                        DrawHelpText("GL_OVR_multiview2 is not available on every desktop GL "
                                     "driver. If it is missing the session runs Multi Pass, and "
                                     "Runtime Diagnostics below reports which one is in use.");
                    }
                }
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "Rendering", "Depth Submission",
                        "depth layer reprojection")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_OPENXR_DEPTH_SUBMISSION", "Depth Submission"),
                    Loc::T("SETTINGS_OPENXR_DEPTH_SUBMISSION_DESC",
                           "Submit a depth layer alongside the projection layer so the runtime can "
                           "reproject more accurately. Needs XR_KHR_composition_layer_depth; falls "
                           "back to None when the runtime does not offer it."),
                    [&]() {
                        int depth = (target.depthSubmission == XR::XRDepthSubmission::Depth) ? 1 : 0;
                        const char* options[] = { "None", "Depth" };
                        if (ImGui::Combo("##OpenXRDepthSubmission", &depth, options, IM_ARRAYSIZE(options))) {
                            target.depthSubmission = (depth == 1) ? XR::XRDepthSubmission::Depth
                                                                  : XR::XRDepthSubmission::None;
                            return true;
                        }
                        return false;
                    });
            }
            ImGui::EndDisabled();
        }

        if (DrawSettingSection("[I]", Loc::T("SETTINGS_OPENXR_INTERACTION_PROFILES", "Interaction Profiles"),
                               Loc::T("SETTINGS_OPENXR_INTERACTION_PROFILES_DESC",
                                      "OpenXR controller profiles this target suggests bindings "
                                      "for. Suggesting one costs nothing when the hardware is "
                                      "absent - the runtime simply never selects it."))) {
            ImGui::BeginDisabled(!targetOn);

            static std::array<int, static_cast<size_t>(XR::XRPlatformTarget::Count)>
                selectedProfileRow = { -1, -1, -1 };
            int& selectedRow = selectedProfileRow[static_cast<size_t>(platformTarget)];
            const int profileCount = static_cast<int>(target.interactionProfiles.size());
            if (selectedRow >= profileCount) selectedRow = profileCount - 1;

            const float listHeight = ImGui::GetTextLineHeightWithSpacing() * 4.0f + 8.0f;
            ImGui::BeginChild("##OpenXRProfileList", ImVec2(0.0f, listHeight), true);
            for (int i = 0; i < profileCount; ++i) {
                const XR::XRInteractionProfile profile = target.interactionProfiles[i];
                ImGui::PushID(i);
                if (ImGui::Selectable(XR::DisplayName(profile), selectedRow == i)) {
                    selectedRow = i;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(XR::InteractionProfilePath(profile));
                    if (!XR::IsInteractionProfileImplemented(profile)) {
                        ImGui::TextDisabled("No binding table exists for this profile yet, so it "
                                            "is skipped at session setup.");
                    } else if (!XR::IsInteractionProfileAvailableOn(profile, platformTarget)) {
                        ImGui::TextDisabled("This controller does not appear on this platform.");
                    }
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
            if (profileCount == 0) {
                ImGui::TextDisabled("No interaction profiles. Controllers will report no input.");
            }
            ImGui::EndChild();

            const float buttonWidth = ImGui::GetFrameHeight() + 6.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                 buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::Button("+##OpenXRAddProfile", ImVec2(buttonWidth, 0.0f))) {
                ImGui::OpenPopup("##OpenXRAddProfilePopup");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Add a controller profile for this target");
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(selectedRow < 0 || selectedRow >= profileCount);
            if (ImGui::Button("-##OpenXRRemoveProfile", ImVec2(buttonWidth, 0.0f)) &&
                selectedRow >= 0 && selectedRow < static_cast<int>(target.interactionProfiles.size())) {
                target.interactionProfiles.erase(target.interactionProfiles.begin() + selectedRow);
                if (selectedRow >= static_cast<int>(target.interactionProfiles.size())) {
                    selectedRow = static_cast<int>(target.interactionProfiles.size()) - 1;
                }
                changed = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Remove the selected profile");
            }

            if (ImGui::BeginPopup("##OpenXRAddProfilePopup")) {
                int addable = 0;
                for (int i = 0; i < static_cast<int>(XR::XRInteractionProfile::Count); ++i) {
                    const auto profile = static_cast<XR::XRInteractionProfile>(i);
                    if (!XR::IsInteractionProfileAvailableOn(profile, platformTarget)) continue;
                    if (target.hasInteractionProfile(profile)) continue;
                    ++addable;
                    const bool implemented = XR::IsInteractionProfileImplemented(profile);
                    ImGui::BeginDisabled(!implemented);
                    if (ImGui::MenuItem(XR::DisplayName(profile))) {
                        target.setInteractionProfile(profile, true);
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    if (!implemented) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(Coming Soon)");
                    }
                }
                if (addable == 0) {
                    ImGui::TextDisabled("Every profile for this platform is already added.");
                }
                ImGui::EndPopup();
            }

            if (target.effectiveInteractionProfiles().empty()) {
                DrawHelpText("No usable interaction profile is enabled, so controllers will report "
                             "no input. Khronos Simple Controller is the safe minimum - every "
                             "conformant runtime supports it.");
            }
            ImGui::EndDisabled();
        }

        if (DrawSettingSection("[F]", Loc::T("SETTINGS_OPENXR_FEATURE_GROUPS", "OpenXR Feature Groups"),
                               Loc::T("SETTINGS_OPENXR_FEATURE_GROUPS_DESC",
                                      "Optional OpenXR extensions to request for this target. "
                                      "Groups on the left, the features in the selected group on "
                                      "the right."))) {
            ImGui::BeginDisabled(!targetOn);

            std::vector<XR::XRFeatureGroup> groups;
            for (int g = 0; g < static_cast<int>(XR::XRFeatureGroup::Count); ++g) {
                const auto group = static_cast<XR::XRFeatureGroup>(g);
                if (XR::IsFeatureGroupUsedOn(group, platformTarget)) groups.push_back(group);
            }

            static std::array<int, static_cast<size_t>(XR::XRPlatformTarget::Count)>
                selectedFeatureGroup = { 0, 0, 0 };
            int& groupRow = selectedFeatureGroup[static_cast<size_t>(platformTarget)];
            if (groupRow >= static_cast<int>(groups.size())) groupRow = 0;

            if (groups.empty()) {
                ImGui::TextDisabled("No optional OpenXR extensions are declared for this platform "
                                    "yet.");
            } else if (ImGui::BeginTable("##OpenXRFeatureGroups", 2,
                                         ImGuiTableFlags_SizingStretchProp |
                                             ImGuiTableFlags_NoSavedSettings)) {
                ImGui::TableSetupColumn("Groups", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableSetupColumn("Features", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                ImGui::TableNextRow();

                const float paneHeight = ImGui::GetTextLineHeightWithSpacing() * 6.0f + 8.0f;

                ImGui::TableSetColumnIndex(0);
                ImGui::BeginChild("##OpenXRGroupList", ImVec2(0.0f, paneHeight), true);
                for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
                    const XR::XRFeatureGroup group = groups[i];
                    ImGui::PushID(i);

                    int implementedCount = 0;
                    int enabledCount = 0;
                    for (const XR::XRFeatureInfo& info : XR::AllFeatures()) {
                        if (info.group != group) continue;
                        if (!XR::IsFeatureAvailableOn(info.feature, platformTarget)) continue;
                        if (!info.implemented) continue;
                        ++implementedCount;
                        if (target.hasFeature(info.serializationId)) ++enabledCount;
                    }
                    bool allOn = implementedCount > 0 && enabledCount == implementedCount;
                    ImGui::BeginDisabled(implementedCount == 0);
                    if (ImGui::Checkbox("##OpenXRGroupToggle", &allOn)) {
                        for (const XR::XRFeatureInfo& info : XR::AllFeatures()) {
                            if (info.group != group) continue;
                            if (!XR::IsFeatureAvailableOn(info.feature, platformTarget)) continue;
                            if (!info.implemented) continue;
                            target.setFeature(info.serializationId, allOn);
                        }
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Selectable(XR::DisplayName(group), groupRow == i)) {
                        groupRow = i;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", XR::Description(group));
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();

                ImGui::TableSetColumnIndex(1);
                ImGui::BeginChild("##OpenXRFeatureList", ImVec2(0.0f, paneHeight), true);
                const XR::XRFeatureGroup activeGroup =
                    groups[static_cast<size_t>(std::max(0, groupRow))];
                for (const XR::XRFeatureInfo& info : XR::AllFeatures()) {
                    if (info.group != activeGroup) continue;
                    if (!XR::IsFeatureAvailableOn(info.feature, platformTarget)) continue;
                    if (!visible(ProjectSettingsVisibilityMode::Simple, "OpenXR Feature Groups",
                                 info.displayName, "extension feature openxr")) {
                        continue;
                    }
                    ImGui::PushID(info.serializationId);
                    bool on = target.hasFeature(info.serializationId);
                    ImGui::BeginDisabled(!info.implemented);
                    if (ImGui::Checkbox(info.displayName, &on)) {
                        target.setFeature(info.serializationId, on);
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", info.extensionName);
                    }
                    if (!info.implemented) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(Coming Soon)");
                    } else if (diag.systemAcquired && !diag.hasExtension(info.extensionName)) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(not offered by the probed runtime)");
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
        }

        if (isPCTab &&
            DrawSettingSection("[PC]", Loc::T("SETTINGS_OPENXR_PC_RUNTIME", "PC Runtime"),
                               Loc::T("SETTINGS_OPENXR_PC_RUNTIME_DESC",
                                      "Which OpenXR runtime the editor and PC builds talk to - "
                                      "SteamVR, Meta Quest Link, Windows Mixed Reality, and so on."))) {
            ImGui::BeginDisabled(!targetOn);

            static std::vector<XR::XRInstalledRuntime> installedRuntimes;
            static bool runtimesScanned = false;
            if (!runtimesScanned) {
                installedRuntimes = XR::EnumerateInstalledRuntimes();
                runtimesScanned = true;
            }

            if (visible(ProjectSettingsVisibilityMode::Simple, "PC Runtime", "OpenXR Runtime",
                        "steamvr oculus meta link wmr virtual desktop runtime")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_OPENXR_PC_RUNTIME_PICK", "OpenXR Runtime"),
                    Loc::T("SETTINGS_OPENXR_PC_RUNTIME_PICK_DESC",
                           "System Default uses whatever the machine is set to. Picking a specific "
                           "runtime sets XR_RUNTIME_JSON for this process only, so the editor talks "
                           "to it without changing the system-wide default for anything else."),
                    [&]() {
                        int current = 0;
                        for (size_t i = 0; i < installedRuntimes.size(); ++i) {
                            if (installedRuntimes[i].manifestPath == xr.pc.runtimeManifestPath) {
                                current = static_cast<int>(i) + 1;
                                break;
                            }
                        }
                        std::string preview = "System Default";
                        if (!xr.pc.runtimeManifestPath.empty()) {
                            preview = xr.pc.runtimeDisplayName.empty()
                                          ? xr.pc.runtimeManifestPath
                                          : xr.pc.runtimeDisplayName;
                            if (current == 0) preview += " (not installed here)";
                        }

                        bool edited = false;
                        if (ImGui::BeginCombo("##OpenXRPCRuntime", preview.c_str())) {
                            if (ImGui::Selectable("System Default", current == 0)) {
                                xr.pc.runtimeManifestPath.clear();
                                xr.pc.runtimeDisplayName.clear();
                                edited = true;
                            }
                            for (size_t i = 0; i < installedRuntimes.size(); ++i) {
                                const XR::XRInstalledRuntime& entry = installedRuntimes[i];
                                ImGui::PushID(static_cast<int>(i));
                                ImGui::BeginDisabled(!entry.manifestExists);
                                std::string label = entry.displayName;
                                if (entry.isSystemDefault) label += "  (system default)";
                                if (!entry.manifestExists) label += "  (missing)";
                                if (ImGui::Selectable(label.c_str(),
                                                      current == static_cast<int>(i) + 1)) {
                                    xr.pc.runtimeManifestPath = entry.manifestPath;
                                    xr.pc.runtimeDisplayName = entry.displayName;
                                    edited = true;
                                }
                                ImGui::EndDisabled();
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                    ImGui::BeginTooltip();
                                    ImGui::TextUnformatted(entry.manifestPath.c_str());
                                    if (!entry.libraryPath.empty()) {
                                        ImGui::TextDisabled("%s", entry.libraryPath.c_str());
                                    }
                                    ImGui::EndTooltip();
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndCombo();
                        }
                        return edited;
                    });

                if (ImGui::Button(Loc::T("SETTINGS_OPENXR_PC_RUNTIME_REFRESH", "Refresh Runtimes"),
                                  ImVec2(170.0f, 0.0f))) {
                    installedRuntimes = XR::EnumerateInstalledRuntimes();
                    addConsoleMessage("Found " + std::to_string(installedRuntimes.size()) +
                                          " installed OpenXR runtime(s).",
                                      ConsoleMessageType::Info);
                }
                ImGui::SameLine();
                if (installedRuntimes.empty()) {
                    ImGui::TextDisabled("No OpenXR runtime is registered on this machine.");
                } else {
                    ImGui::TextDisabled("%d installed.", static_cast<int>(installedRuntimes.size()));
                }

                if (openXrCompiledIn && XR::HasLoadedRuntimeThisSession() &&
                    XR::ActiveRuntimeOverride() != xr.pc.runtimeManifestPath) {
                    DrawHelpText("An OpenXR runtime has already been loaded in this editor "
                                 "session. Restart Modularity for this selection to take effect.");
                }
            }

            if (visible(ProjectSettingsVisibilityMode::Simple, "PC Runtime", "Start VR on Play",
                        "play mode editor headset preview")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_OPENXR_PC_START_ON_PLAY", "Start VR on Play"),
                    Loc::T("SETTINGS_OPENXR_PC_START_ON_PLAY_DESC",
                           "Bring the headset session up when the editor enters Play mode. Turn "
                           "this off to preview flat in the editor and only go to the headset in a "
                           "built player - useful when the headset is busy or not plugged in. "
                           "A built PC player always starts VR."),
                    [&]() { return ImGui::Checkbox("##OpenXRPCStartOnPlay", &xr.pc.startOnPlay); });
            }

            DrawHelpText("Windows ships no system-wide OpenXR loader: openxr_loader.dll travels "
                         "with the application. If Runtime Diagnostics reports no loader, put it "
                         "next to Modularity.exe or in redist/windows-x64/ - "
                         "tools/fetch-openxr-loader-windows.sh does it for you.");
            ImGui::EndDisabled();
        }

        if (DrawSettingSection("[N]", Loc::T("SETTINGS_OPENXR_XR_INTERACTION", "XR Interaction"),
                               Loc::T("SETTINGS_OPENXR_XR_INTERACTION_DESC",
                                      "The interactor / interactable component layer. Independent of "
                                      "OpenXR itself: scripts can use ModuInput.XR without it."))) {
            ImGui::BeginDisabled(!xrOn);
            if (visible(ProjectSettingsVisibilityMode::Simple, "XR Interaction",
                        "Enable XR Interaction System", "interactor interactable grab ray")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_OPENXR_INTERACTION_ENABLE", "Enable XR Interaction System"),
                    Loc::T("SETTINGS_OPENXR_INTERACTION_ENABLE_DESC",
                           "Run the XR Interaction Manager so ray, direct and socket interactors "
                           "can hover, select and activate interactables."),
                    [&]() { return ImGui::Checkbox("##OpenXRInteractionEnabled", &xr.interaction.enabled); });
            }
            if (visible(ProjectSettingsVisibilityMode::Advanced, "XR Interaction", "Default Action Set",
                        "actions bindings remap")) {
                changed |= DrawSettingRow(
                    Loc::T("SETTINGS_OPENXR_DEFAULT_ACTION_SET", "Default Action Set"),
                    Loc::T("SETTINGS_OPENXR_DEFAULT_ACTION_SET_DESC",
                           "Name of the action set driving XR components. Leave empty to use the "
                           "built-in \"Default XR Actions\" set."),
                    [&]() {
                        char buffer[128];
                        std::snprintf(buffer, sizeof(buffer), "%s", xr.interaction.defaultActionSet.c_str());
                        if (ImGui::InputTextWithHint("##OpenXRDefaultActionSet", "Default XR Actions",
                                                     buffer, sizeof(buffer))) {
                            xr.interaction.defaultActionSet = buffer;
                            return true;
                        }
                        return false;
                    });
            }
            ImGui::EndDisabled();
        }

        if (DrawSettingSection("[D]", Loc::T("SETTINGS_OPENXR_RUNTIME_DIAGNOSTICS", "Runtime Diagnostics"),
                               Loc::T("SETTINGS_OPENXR_RUNTIME_DIAGNOSTICS_DESC",
                                      "What an OpenXR runtime on this machine actually reports. "
                                      "Nothing here is a project setting."))) {
            if (!openXrCompiledIn) {
                ImGui::TextDisabled("OpenXR is not compiled into this build "
                                    "(MODULARITY_ENABLE_OPENXR=OFF).");
            } else {
                if (ImGui::Button(Loc::T("SETTINGS_OPENXR_PROBE", "Probe OpenXR Runtime"),
                                  ImVec2(200.0f, 0.0f))) {
                    XR::XRRuntime probe;
                    std::string probeError;
                    void* vm = nullptr;
                    void* activity = nullptr;
#ifdef __ANDROID__
                    vm = Modularity::AndroidRuntime::GetJavaVM();
                    activity = Modularity::AndroidRuntime::GetActivityObject();
#endif
                    if (XR::ActivePlatformTarget() == XR::XRPlatformTarget::PC) {
                        std::string overrideError;
                        if (!XR::ApplyRuntimeOverride(xr.pc.runtimeManifestPath, overrideError) &&
                            !overrideError.empty()) {
                            addConsoleMessage("OpenXR runtime selection: " + overrideError,
                                              ConsoleMessageType::Warning);
                        }
                        XR::MutableDiagnostics().runtimeOverridePath = XR::ActiveRuntimeOverride();
                    }

                    XR::ProjectOpenXRSettings probeSettings = xr;
                    probeSettings.enabled = true;
                    probeSettings.activeTarget().enabled = true;
                    if (probe.initialize(probeSettings, vm, activity, probeError)) {
                        addConsoleMessage("OpenXR runtime probed: " +
                                              (probe.runtimeName().empty() ? std::string("(unnamed runtime)")
                                                                           : probe.runtimeName()),
                                          ConsoleMessageType::Success);
                    } else {
                        addConsoleMessage("OpenXR probe failed: " + probeError,
                                          ConsoleMessageType::Warning);
                    }
                    probe.shutdown();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Starts and stops an OpenXR instance to read the runtime's own report.");

                auto diagRow = [](const char* label, const std::string& value) {
                    DrawSettingRow(label, nullptr, [&]() {
                        ImGui::TextUnformatted(value.empty() ? "-" : value.c_str());
                        return false;
                    });
                };
                auto boolText = [](bool supported, bool known) {
                    if (!known) return std::string("Unknown");
                    return std::string(supported ? "Supported" : "Not supported");
                };

                if (!diag.lastError.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
                    ImGui::TextWrapped("%s", diag.lastError.c_str());
                    ImGui::PopStyleColor();
                }
                diagRow("Platform Target", diag.platformTarget.empty()
                                               ? std::string(XR::DisplayName(XR::ActivePlatformTarget()))
                                               : diag.platformTarget);
                diagRow("Loader", diag.loaderAvailable ? diag.loaderPath : std::string("Not found"));
                diagRow("Runtime Override", diag.runtimeOverridePath.empty()
                                                ? std::string("System default")
                                                : diag.runtimeOverridePath);
                diagRow("Runtime", diag.runtimeName);
                diagRow("Runtime Version", diag.runtimeVersion);
                diagRow("OpenXR Version", diag.openXRVersion);
                diagRow("System", diag.systemName);
                diagRow("Graphics API", diag.graphicsApi);
                diagRow("Graphics Context", diag.graphicsVersion);
                diagRow("View Configuration", diag.viewConfiguration);
                diagRow("Views", diag.viewCount ? std::to_string(diag.viewCount) : std::string());
                diagRow("Recommended Eye Resolution",
                        diag.recommendedWidth
                            ? std::to_string(diag.recommendedWidth) + " x " +
                                  std::to_string(diag.recommendedHeight)
                            : std::string());
                diagRow("Maximum Eye Resolution",
                        diag.maxWidth ? std::to_string(diag.maxWidth) + " x " +
                                            std::to_string(diag.maxHeight)
                                      : std::string());
                diagRow("Multiview", boolText(diag.multiviewSupported, diag.sessionCreated));
                diagRow("Depth Submission", boolText(diag.depthSubmissionSupported, diag.instanceCreated));
                diagRow("Hand Tracking", boolText(diag.handTrackingSupported, diag.instanceCreated));
                diagRow("Passthrough", boolText(diag.passthroughSupported, diag.instanceCreated));
                diagRow("Active Render Mode", diag.activeRenderMode);
                diagRow("Active Tracking Origin", diag.activeTrackingOrigin);

                if (!diag.blendModes.empty()) {
                    std::string modes;
                    for (const std::string& mode : diag.blendModes) {
                        if (!modes.empty()) modes += ", ";
                        modes += mode;
                    }
                    diagRow("Blend Modes", modes);
                }
                if (!diag.availableExtensions.empty() &&
                    ImGui::TreeNode("Available Extensions##OpenXRExtensions")) {
                    for (const std::string& name : diag.availableExtensions) {
                        const bool enabled = diag.isExtensionEnabled(name);
                        if (enabled) {
                            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.0f), "%s (enabled)",
                                               name.c_str());
                        } else {
                            ImGui::TextDisabled("%s", name.c_str());
                        }
                    }
                    ImGui::TreePop();
                }
            }
        }

        if (changed) projectManager.currentProject.saveProjectFile();
    }

    EndProjectSettingsColumns();

    ImGui::PopStyleVar(2);
    ImGui::EndChild();

    ImGui::End();
    if (wasOpen != showProjectBrowser) {
        saveEditorUserSettings();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
}
#pragma endregion
