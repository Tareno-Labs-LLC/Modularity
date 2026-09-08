// THIS CODE IS UNUSED: i don't really plan to add this any time soon lol.

#include "../EditorLocalization.h"
#include "../Engine.h"

#include <cmath>

namespace Loc = Modularity::Loc;

namespace {
enum class VisualItemKind {
    Block,
    Node
};

struct VisualGraphItem {
    int id = 0;
    VisualItemKind kind = VisualItemKind::Block;
    std::string title;
    std::string detail;
    ImVec2 pos;
    ImVec2 size;
    ImU32 color = IM_COL32_WHITE;
    double createdAt = 0.0;
};

struct VisualGraphLink {
    int from = 0;
    int to = 0;
    ImU32 color = IM_COL32(142, 177, 255, 210);
};

struct VisualScriptingState {
    std::vector<VisualGraphItem> items;
    std::vector<VisualGraphLink> links;
    int nextId = 1;
    int selectedId = -1;
    int draggingId = -1;
    ImVec2 dragOffset = ImVec2(0.0f, 0.0f);
    ImVec2 pan = ImVec2(360.0f, 180.0f);
    ImVec2 targetPan = ImVec2(360.0f, 180.0f);
    ImVec2 lastCanvasMin = ImVec2(0.0f, 0.0f);
    ImVec2 lastCanvasSize = ImVec2(0.0f, 0.0f);
    float zoom = 1.0f;
    float targetZoom = 1.0f;
    bool initialized = false;
};

VisualScriptingState& GetVisualScriptingState() {
    static VisualScriptingState state;
    return state;
}

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

float EaseOutBack(float t) {
    t = ClampFloat(t, 0.0f, 1.0f);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

ImVec2 Add(const ImVec2& a, const ImVec2& b) {
    return ImVec2(a.x + b.x, a.y + b.y);
}

ImVec2 Sub(const ImVec2& a, const ImVec2& b) {
    return ImVec2(a.x - b.x, a.y - b.y);
}

ImVec2 Mul(const ImVec2& a, float s) {
    return ImVec2(a.x * s, a.y * s);
}

bool Contains(const ImVec2& min, const ImVec2& max, const ImVec2& point) {
    return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
}

ImU32 WithAlpha(ImU32 color, int alpha) {
    return (color & IM_COL32(255, 255, 255, 0)) | IM_COL32(0, 0, 0, alpha);
}

ImVec2 WorldToScreen(const VisualScriptingState& state, const ImVec2& origin, const ImVec2& world) {
    return Add(origin, Add(state.pan, Mul(world, state.zoom)));
}

ImVec2 ScreenToWorld(const VisualScriptingState& state, const ImVec2& origin, const ImVec2& screen) {
    return Mul(Sub(Sub(screen, origin), state.pan), 1.0f / state.zoom);
}

VisualGraphItem* FindItem(VisualScriptingState& state, int id) {
    for (VisualGraphItem& item : state.items) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

const VisualGraphItem* FindItem(const VisualScriptingState& state, int id) {
    for (const VisualGraphItem& item : state.items) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

void AddItem(VisualScriptingState& state,
             VisualItemKind kind,
             const char* title,
             const char* detail,
             ImVec2 pos,
             ImVec2 size,
             ImU32 color) {
    VisualGraphItem item;
    item.id = state.nextId++;
    item.kind = kind;
    item.title = title;
    item.detail = detail;
    item.pos = pos;
    item.size = size;
    item.color = color;
    item.createdAt = ImGui::GetTime();
    state.items.push_back(item);
    state.selectedId = item.id;
}

void ResetDemoGraph(VisualScriptingState& state) {
    state.items.clear();
    state.links.clear();
    state.nextId = 1;
    AddItem(state, VisualItemKind::Block, "When Game Starts", "entry event", ImVec2(-330.0f, -90.0f), ImVec2(230.0f, 72.0f), IM_COL32(255, 193, 33, 255));
    AddItem(state, VisualItemKind::Block, "Forever", "main update loop", ImVec2(-80.0f, -70.0f), ImVec2(210.0f, 108.0f), IM_COL32(255, 146, 24, 255));
    AddItem(state, VisualItemKind::Block, "If Key Pressed", "Space", ImVec2(155.0f, -64.0f), ImVec2(230.0f, 96.0f), IM_COL32(255, 122, 42, 255));
    AddItem(state, VisualItemKind::Node, "Get Player", "external scene reference", ImVec2(-42.0f, 145.0f), ImVec2(210.0f, 118.0f), IM_COL32(80, 144, 255, 255));
    AddItem(state, VisualItemKind::Node, "Add Velocity", "physics call", ImVec2(230.0f, 130.0f), ImVec2(220.0f, 128.0f), IM_COL32(116, 95, 255, 255));
    state.links.push_back({1, 2, IM_COL32(255, 214, 91, 230)});
    state.links.push_back({2, 3, IM_COL32(255, 168, 84, 230)});
    state.links.push_back({3, 5, IM_COL32(255, 168, 84, 230)});
    state.links.push_back({4, 5, IM_COL32(118, 174, 255, 230)});
    state.selectedId = -1;
}

void EnsureInitialized(VisualScriptingState& state) {
    if (state.initialized) return;
    state.initialized = true;
    ResetDemoGraph(state);
}

void DrawGrid(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const VisualScriptingState& state) {
    drawList->AddRectFilled(min, max, IM_COL32(19, 22, 28, 255));
    const float smallStep = 32.0f * state.zoom;
    const float largeStep = smallStep * 4.0f;
    auto drawGridSet = [&](float step, ImU32 color, float thickness) {
        if (step < 8.0f) return;
        const float startX = min.x + std::fmod(state.pan.x, step);
        const float startY = min.y + std::fmod(state.pan.y, step);
        for (float x = startX; x < max.x; x += step) {
            drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), color, thickness);
        }
        for (float y = startY; y < max.y; y += step) {
            drawList->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), color, thickness);
        }
    };
    drawGridSet(smallStep, IM_COL32(255, 255, 255, 16), 1.0f);
    drawGridSet(largeStep, IM_COL32(255, 255, 255, 30), 1.0f);
}

void DrawTextScaled(ImDrawList* drawList, const ImVec2& pos, ImU32 color, float size, const char* text) {
    drawList->AddText(ImGui::GetFont(), size, pos, color, text);
}

void DrawSoftShadow(ImDrawList* drawList,
                    const ImVec2& min,
                    const ImVec2& max,
                    float rounding,
                    float scale,
                    int alpha) {
    for (int i = 4; i >= 1; --i) {
        const float spread = static_cast<float>(i) * 4.5f * scale;
        const float yOffset = static_cast<float>(i) * 1.3f * scale;
        const int layerAlpha = alpha / (i + 2);
        drawList->AddRectFilled(ImVec2(min.x - spread, min.y - spread + yOffset),
                                ImVec2(max.x + spread, max.y + spread + yOffset),
                                IM_COL32(0, 0, 0, layerAlpha),
                                rounding + spread);
    }
}

void DrawBlock(ImDrawList* drawList,
               const VisualGraphItem& item,
               const VisualScriptingState& state,
               const ImVec2& origin,
               bool selected,
               double now) {
    const float age = static_cast<float>(now - item.createdAt);
    const float pop = age < 0.34f ? EaseOutBack(age / 0.34f) : 1.0f;
    const float zoom = state.zoom * pop;
    const ImVec2 center = WorldToScreen(state, origin, Add(item.pos, Mul(item.size, 0.5f)));
    const ImVec2 size = Mul(item.size, zoom);
    const ImVec2 min(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
    const ImVec2 max(center.x + size.x * 0.5f, center.y + size.y * 0.5f);
    const float rounding = 14.0f * state.zoom;
    DrawSoftShadow(drawList, min, max, rounding, state.zoom, 130);
    drawList->AddRectFilled(min, max, item.color, rounding);
    drawList->AddRectFilled(ImVec2(min.x, min.y), ImVec2(max.x, min.y + 26.0f * state.zoom), IM_COL32(255, 255, 255, 32), rounding, ImDrawFlags_RoundCornersTop);
    drawList->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 240) : IM_COL32(255, 255, 255, 64), rounding, 0, selected ? 2.8f : 1.2f);

    const float notchW = 42.0f * state.zoom;
    const float notchH = 12.0f * state.zoom;
    const float notchX = min.x + 30.0f * state.zoom;
    drawList->AddRectFilled(ImVec2(notchX, min.y - 1.0f), ImVec2(notchX + notchW, min.y + notchH), IM_COL32(19, 22, 28, 255), 6.0f * state.zoom);
    drawList->AddRectFilled(ImVec2(notchX, max.y - notchH), ImVec2(notchX + notchW, max.y + 1.0f), WithAlpha(item.color, 255), 6.0f * state.zoom);

    const float titleSize = 17.0f * state.zoom;
    const float detailSize = 13.0f * state.zoom;
    DrawTextScaled(drawList, ImVec2(min.x + 18.0f * state.zoom, min.y + 13.0f * state.zoom), IM_COL32(24, 24, 26, 245), titleSize, item.title.c_str());
    DrawTextScaled(drawList, ImVec2(min.x + 18.0f * state.zoom, min.y + 42.0f * state.zoom), IM_COL32(38, 39, 43, 220), detailSize, item.detail.c_str());
}

void DrawNode(ImDrawList* drawList,
              const VisualGraphItem& item,
              const VisualScriptingState& state,
              const ImVec2& origin,
              bool selected,
              double now) {
    const float age = static_cast<float>(now - item.createdAt);
    const float pop = age < 0.34f ? EaseOutBack(age / 0.34f) : 1.0f;
    const float zoom = state.zoom * pop;
    const ImVec2 center = WorldToScreen(state, origin, Add(item.pos, Mul(item.size, 0.5f)));
    const ImVec2 size = Mul(item.size, zoom);
    const ImVec2 min(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
    const ImVec2 max(center.x + size.x * 0.5f, center.y + size.y * 0.5f);
    const float rounding = 9.0f * state.zoom;
    DrawSoftShadow(drawList, min, max, rounding, state.zoom, 150);
    drawList->AddRectFilled(min, max, IM_COL32(38, 42, 53, 250), rounding);
    drawList->AddRectFilled(min, ImVec2(max.x, min.y + 34.0f * state.zoom), item.color, rounding, ImDrawFlags_RoundCornersTop);
    drawList->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 56), rounding, 0, selected ? 2.6f : 1.1f);

    DrawTextScaled(drawList, ImVec2(min.x + 15.0f * state.zoom, min.y + 9.0f * state.zoom), IM_COL32(255, 255, 255, 245), 15.5f * state.zoom, item.title.c_str());
    DrawTextScaled(drawList, ImVec2(min.x + 15.0f * state.zoom, min.y + 52.0f * state.zoom), IM_COL32(190, 199, 216, 230), 13.0f * state.zoom, item.detail.c_str());

    const float pinRadius = 6.0f * state.zoom;
    const ImVec2 inPin(min.x, min.y + size.y * 0.5f);
    const ImVec2 outPin(max.x, min.y + size.y * 0.5f);
    drawList->AddCircleFilled(inPin, pinRadius, IM_COL32(95, 231, 142, 255));
    drawList->AddCircleFilled(outPin, pinRadius, IM_COL32(95, 184, 255, 255));
    drawList->AddCircle(inPin, pinRadius + 2.0f * state.zoom, IM_COL32(255, 255, 255, 90), 16, 1.0f);
    drawList->AddCircle(outPin, pinRadius + 2.0f * state.zoom, IM_COL32(255, 255, 255, 90), 16, 1.0f);
}

void DrawLinks(ImDrawList* drawList, const VisualScriptingState& state, const ImVec2& origin) {
    for (const VisualGraphLink& link : state.links) {
        const VisualGraphItem* from = FindItem(state, link.from);
        const VisualGraphItem* to = FindItem(state, link.to);
        if (!from || !to) continue;
        const ImVec2 a = WorldToScreen(state, origin, ImVec2(from->pos.x + from->size.x, from->pos.y + from->size.y * 0.5f));
        const ImVec2 b = WorldToScreen(state, origin, ImVec2(to->pos.x, to->pos.y + to->size.y * 0.5f));
        const float tension = std::max(80.0f * state.zoom, std::abs(b.x - a.x) * 0.46f);
        drawList->AddBezierCubic(a,
                                 ImVec2(a.x + tension, a.y),
                                 ImVec2(b.x - tension, b.y),
                                 b,
                                 IM_COL32(0, 0, 0, 120),
                                 5.0f * state.zoom,
                                 28);
        drawList->AddBezierCubic(a,
                                 ImVec2(a.x + tension, a.y),
                                 ImVec2(b.x - tension, b.y),
                                 b,
                                 link.color,
                                 2.6f * state.zoom,
                                 28);
    }
}

void DrawPaletteButton(const char* label, ImU32 color, bool nodeStyle, const ImVec2& size) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    drawList->AddRectFilled(min, max, nodeStyle ? IM_COL32(42, 47, 59, 255) : color, 8.0f);
    drawList->AddRectFilled(min, ImVec2(max.x, min.y + 22.0f), nodeStyle ? color : IM_COL32(255, 255, 255, 34), 8.0f, ImDrawFlags_RoundCornersTop);
    drawList->AddRect(min, max, hovered ? IM_COL32(255, 255, 255, 160) : IM_COL32(255, 255, 255, 42), 8.0f, 0, hovered ? 2.0f : 1.0f);
    drawList->AddText(ImVec2(min.x + 12.0f, min.y + 12.0f), nodeStyle ? IM_COL32(245, 248, 255, 255) : IM_COL32(25, 25, 28, 245), label);
}
} // namespace

void Engine::renderVisualScriptingWindow() {
    if (!showVisualScriptingWindow) return;

    VisualScriptingState& state = GetVisualScriptingState();
    EnsureInitialized(state);

    ImGui::SetNextWindowSize(ImVec2(1180.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (mainDockspaceId != 0) {
        ImGui::SetNextWindowDockID(mainDockspaceId, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(Loc::Window("WINDOW_VISUAL_SCRIPT_GRAPH", "Visual Script Graph"), &showVisualScriptingWindow, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const float dt = std::max(io.DeltaTime, 1.0f / 240.0f);
    const float zoomBlend = 1.0f - std::exp(-dt * 14.0f);
    const float panBlend = 1.0f - std::exp(-dt * 16.0f);
    state.zoom += (state.targetZoom - state.zoom) * zoomBlend;
    state.pan.x += (state.targetPan.x - state.pan.x) * panBlend;
    state.pan.y += (state.targetPan.y - state.pan.y) * panBlend;

    ImGui::BeginChild("VisualScriptPalette", ImVec2(238.0f, 0.0f), true);
    ImGui::TextUnformatted("Blocks");
    ImGui::Spacing();
    const ImVec2 paletteButtonSize(196.0f, 52.0f);
    auto addAtCenter = [&](VisualItemKind kind, const char* title, const char* detail, ImVec2 size, ImU32 color) {
        const ImVec2 canvasMin = state.lastCanvasMin.x != 0.0f || state.lastCanvasMin.y != 0.0f
            ? state.lastCanvasMin
            : ImGui::GetMainViewport()->WorkPos;
        const ImVec2 canvasSize = state.lastCanvasSize.x > 1.0f && state.lastCanvasSize.y > 1.0f
            ? state.lastCanvasSize
            : ImGui::GetMainViewport()->WorkSize;
        const ImVec2 canvasCenter(canvasMin.x + canvasSize.x * 0.5f, canvasMin.y + canvasSize.y * 0.5f);
        const ImVec2 world = ScreenToWorld(state, canvasMin, canvasCenter);
        AddItem(state, kind, title, detail, ImVec2(world.x - size.x * 0.5f, world.y - size.y * 0.5f), size, color);
    };
    DrawPaletteButton("When Started", IM_COL32(255, 193, 33, 255), false, paletteButtonSize);
    if (ImGui::IsItemClicked()) addAtCenter(VisualItemKind::Block, "When Game Starts", "entry event", ImVec2(230.0f, 72.0f), IM_COL32(255, 193, 33, 255));
    DrawPaletteButton("Forever", IM_COL32(255, 146, 24, 255), false, paletteButtonSize);
    if (ImGui::IsItemClicked()) addAtCenter(VisualItemKind::Block, "Forever", "main update loop", ImVec2(210.0f, 108.0f), IM_COL32(255, 146, 24, 255));
    DrawPaletteButton("If / Else", IM_COL32(255, 122, 42, 255), false, paletteButtonSize);
    if (ImGui::IsItemClicked()) addAtCenter(VisualItemKind::Block, "If Condition", "branch block", ImVec2(230.0f, 96.0f), IM_COL32(255, 122, 42, 255));
    DrawPaletteButton("Set Variable", IM_COL32(255, 101, 179, 255), false, paletteButtonSize);
    if (ImGui::IsItemClicked()) addAtCenter(VisualItemKind::Block, "Set Variable", "health = 100", ImVec2(230.0f, 78.0f), IM_COL32(255, 101, 179, 255));
    DrawPaletteButton("Move Actor", IM_COL32(70, 196, 108, 255), false, paletteButtonSize);
    if (ImGui::IsItemClicked()) addAtCenter(VisualItemKind::Block, "Move Actor", "x + 1 each tick", ImVec2(230.0f, 78.0f), IM_COL32(70, 196, 108, 255));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("External Nodes");
    ImGui::Spacing();
    DrawPaletteButton("Get Player", IM_COL32(80, 144, 255, 255), true, paletteButtonSize);
    if (ImGui::IsItemClicked()) addAtCenter(VisualItemKind::Node, "Get Player", "scene reference", ImVec2(210.0f, 118.0f), IM_COL32(80, 144, 255, 255));
    DrawPaletteButton("Play Sound", IM_COL32(126, 94, 255, 255), true, paletteButtonSize);
    if (ImGui::IsItemClicked()) addAtCenter(VisualItemKind::Node, "Play Sound", "AudioSystem call", ImVec2(210.0f, 118.0f), IM_COL32(126, 94, 255, 255));
    DrawPaletteButton("Raycast", IM_COL32(86, 211, 173, 255), true, paletteButtonSize);
    if (ImGui::IsItemClicked()) addAtCenter(VisualItemKind::Node, "Raycast", "PhysicsSystem query", ImVec2(210.0f, 118.0f), IM_COL32(86, 211, 173, 255));
    DrawPaletteButton("Emit Event", IM_COL32(241, 104, 104, 255), true, paletteButtonSize);
    if (ImGui::IsItemClicked()) addAtCenter(VisualItemKind::Node, "Emit Event", "send message", ImVec2(210.0f, 118.0f), IM_COL32(241, 104, 104, 255));

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 86.0f);
    if (ImGui::Button("Reset Demo", ImVec2(196.0f, 30.0f))) {
        ResetDemoGraph(state);
    }
    ImGui::TextDisabled("Wheel: smooth zoom");
    ImGui::TextDisabled("Right/middle drag: pan");
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("VisualScriptCanvasHost", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
    state.lastCanvasMin = canvasMin;
    state.lastCanvasSize = canvasSize;
    ImGui::InvisibleButton("##VisualScriptCanvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::PopStyleVar();

    const bool canvasHovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(canvasMin, canvasMax, true);
    DrawGrid(drawList, canvasMin, canvasMax, state);

    if (canvasHovered && std::abs(io.MouseWheel) > 0.0f) {
        const float oldTarget = state.targetZoom;
        const ImVec2 before = ScreenToWorld(state, canvasMin, io.MousePos);
        state.targetZoom = ClampFloat(state.targetZoom * std::pow(1.14f, io.MouseWheel), 0.35f, 2.35f);
        const float ratio = state.targetZoom / std::max(0.001f, oldTarget);
        state.targetPan.x = io.MousePos.x - canvasMin.x - (io.MousePos.x - canvasMin.x - state.targetPan.x) * ratio;
        state.targetPan.y = io.MousePos.y - canvasMin.y - (io.MousePos.y - canvasMin.y - state.targetPan.y) * ratio;
        (void)before;
    }

    if (canvasHovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        state.targetPan = Add(state.targetPan, io.MouseDelta);
        state.pan = Add(state.pan, Mul(io.MouseDelta, 0.55f));
    }

    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.draggingId = -1;
        state.selectedId = -1;
        for (auto it = state.items.rbegin(); it != state.items.rend(); ++it) {
            const ImVec2 min = WorldToScreen(state, canvasMin, it->pos);
            const ImVec2 max = WorldToScreen(state, canvasMin, Add(it->pos, it->size));
            if (Contains(min, max, io.MousePos)) {
                state.selectedId = it->id;
                state.draggingId = it->id;
                state.dragOffset = Sub(ScreenToWorld(state, canvasMin, io.MousePos), it->pos);
                break;
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        state.draggingId = -1;
    }
    if (state.draggingId >= 0) {
        if (VisualGraphItem* item = FindItem(state, state.draggingId)) {
            item->pos = Sub(ScreenToWorld(state, canvasMin, io.MousePos), state.dragOffset);
        }
    }

    DrawLinks(drawList, state, canvasMin);
    const double now = ImGui::GetTime();
    for (const VisualGraphItem& item : state.items) {
        const bool selected = item.id == state.selectedId;
        if (item.kind == VisualItemKind::Block) {
            DrawBlock(drawList, item, state, canvasMin, selected, now);
        } else {
            DrawNode(drawList, item, state, canvasMin, selected, now);
        }
    }

    const ImVec2 toolbarMin(canvasMin.x + 16.0f, canvasMin.y + 14.0f);
    const ImVec2 toolbarMax(toolbarMin.x + 330.0f, toolbarMin.y + 42.0f);
    drawList->AddRectFilled(toolbarMin, toolbarMax, IM_COL32(28, 32, 41, 232), 8.0f);
    drawList->AddRect(toolbarMin, toolbarMax, IM_COL32(255, 255, 255, 38), 8.0f);
    char zoomLabel[64];
    std::snprintf(zoomLabel, sizeof(zoomLabel), "Visual Script   %.0f%%", state.zoom * 100.0f);
    DrawTextScaled(drawList, ImVec2(toolbarMin.x + 14.0f, toolbarMin.y + 12.0f), IM_COL32(235, 240, 250, 245), 15.0f, zoomLabel);

    if (state.selectedId >= 0) {
        const VisualGraphItem* item = FindItem(state, state.selectedId);
        if (item) {
            const ImVec2 infoMin(canvasMax.x - 272.0f, canvasMin.y + 14.0f);
            const ImVec2 infoMax(canvasMax.x - 16.0f, canvasMin.y + 90.0f);
            drawList->AddRectFilled(infoMin, infoMax, IM_COL32(28, 32, 41, 232), 8.0f);
            drawList->AddRect(infoMin, infoMax, item->color, 8.0f, 0, 1.5f);
            DrawTextScaled(drawList, ImVec2(infoMin.x + 14.0f, infoMin.y + 12.0f), IM_COL32(245, 248, 255, 245), 15.0f, item->title.c_str());
            DrawTextScaled(drawList, ImVec2(infoMin.x + 14.0f, infoMin.y + 40.0f), IM_COL32(183, 193, 210, 230), 13.0f, item->detail.c_str());
        }
    }

    drawList->PopClipRect();
    ImGui::EndChild();
    ImGui::End();
}
