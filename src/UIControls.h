#pragma once

#include "SceneObject.h"
#include "UILayout.h"
#include "ThirdParty/ModuGUI/imgui.h"
#include "ThirdParty/ModuGUI/imgui_internal.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <numeric>
#include <string_view>

// Shared by the scene, game, player and offscreen-canvas renderers. These are
// scene components rendered through the same ImGui draw lists as UIButton.
namespace UIControls {
inline void ForwardKeyboardInput(ImGuiIO& destination, const ImGuiIO& source, bool focused) {
    destination.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    destination.BackendFlags = (destination.BackendFlags & ~ImGuiBackendFlags_HasGamepad) |
        (source.BackendFlags & ImGuiBackendFlags_HasGamepad);
    // Mouse aliases are derived by ImGui; submitting them as keys is invalid.
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_MouseLeft; ++key) {
        const ImGuiKeyData& data = source.KeysData[key - ImGuiKey_NamedKey_BEGIN];
        destination.AddKeyAnalogEvent(static_cast<ImGuiKey>(key), focused && data.Down, focused ? data.AnalogValue : 0);
    }
    destination.AddKeyEvent(ImGuiMod_Ctrl, focused && source.KeyCtrl);
    destination.AddKeyEvent(ImGuiMod_Shift, focused && source.KeyShift);
    destination.AddKeyEvent(ImGuiMod_Alt, focused && source.KeyAlt);
    destination.AddKeyEvent(ImGuiMod_Super, focused && source.KeySuper);
    if (focused) for (ImWchar character : source.InputQueueCharacters) destination.AddInputCharacter(character);
}
inline const char* TypeName(UIElementType type) {
    static const char* names[] = {"Panel", "Input Field", "Toggle", "Radio Group", "Progress Bar",
        "Range Slider", "Separator", "Search Field", "Scroll View", "Scrollbar", "Dropdown",
        "List View", "Grid View", "Tree View", "Multi-Column List View", "Tab View", "Foldout",
        "Toolbar", "Breadcrumb", "Menu Bar", "Popup Menu", "Modal", "Color Picker", "Numeric Field",
        "Vector2 Editor", "Vector3 Editor", "Vector4 Editor", "Enum Dropdown", "Reorderable List",
        "Object Reference Field", "Log View", "Split View", "Property Grid", "File / Folder Browser", "Tooltip", "Dockable Panel Container"};
    return IsExtendedUIType(type) ? names[static_cast<int>(type) - static_cast<int>(UIElementType::Panel)] : "UI";
}
inline glm::vec2 DefaultSize(UIElementType type) {
    switch (type) {
    case UIElementType::Panel: case UIElementType::ScrollView: case UIElementType::ListView:
    case UIElementType::TreeView: case UIElementType::GridView: case UIElementType::MultiColumnListView:
    case UIElementType::ReorderableList: case UIElementType::LogView:
    case UIElementType::TabView: case UIElementType::PropertyGrid: case UIElementType::SplitView: return glm::vec2(320, 240);
    case UIElementType::ColorPicker: return glm::vec2(280, 300);
    case UIElementType::FileBrowser: return glm::vec2(340, 260);
    case UIElementType::DockContainer: return glm::vec2(640, 400);
    case UIElementType::Modal: return glm::vec2(320, 160);
    case UIElementType::RadioGroup: return glm::vec2(200, 96);
    case UIElementType::Separator: return glm::vec2(240, 2);
    case UIElementType::Scrollbar: return glm::vec2(14, 200);
    case UIElementType::Vector3Field: return glm::vec2(320, 28);
    case UIElementType::Vector4Field: return glm::vec2(380, 28);
    case UIElementType::Toolbar: case UIElementType::Breadcrumb: case UIElementType::MenuBar: return glm::vec2(320, 30);
    case UIElementType::Tooltip: return glm::vec2(120, 32);
    default: return glm::vec2(240, 32);
    }
}

// Colors of the editor theme (applyModularityStyle in EditorUI.cpp). New controls start
// from these, and unset (alpha 0) background/border/text overrides derive from them.
namespace Theme {
inline const glm::vec4 Accent(0.580f, 0.451f, 0.902f, 1.0f);
inline const glm::vec4 Frame(0.094f, 0.082f, 0.129f, 1.0f);
inline const glm::vec4 Panel(0.118f, 0.102f, 0.161f, 1.0f);
inline const glm::vec4 Outline(0.271f, 0.239f, 0.376f, 0.85f);
inline const glm::vec4 Text(0.894f, 0.882f, 0.925f, 1.0f);
} // namespace Theme

// Starting state for a control created from the editor menu, the Add Component menu or a
// template. Only appearance and starter content: position, anchor and hierarchy are left alone.
inline void ApplyCreationDefaults(UIElementComponent& u, UIElementType type) {
    u.type = type;
    if (!IsExtendedUIType(type)) return;
    u.size = DefaultSize(type);
    u.label = TypeName(type);
    u.color = Theme::Accent;
    u.fillColor = u.backgroundColor = u.borderColor = u.textColor = glm::vec4(0.0f);
    u.rounding = 6.0f; u.borderWidth = 1.0f;
    u.placeholder = "Select...";
    u.items.clear(); u.itemParents.clear(); u.expandedItems.clear();
    u.selectedIndex = -1; u.rowHeight = 26.0f;
    u.numericValue = glm::vec4(0.0f);
    auto numbered = [&](const char* prefix, int count) {
        for (int i = 1; i <= count; ++i) u.items.push_back(prefix + std::to_string(i));
    };
    switch (type) {
    case UIElementType::Panel: case UIElementType::ScrollView: case UIElementType::DockContainer:
        u.color = Theme::Panel; u.borderColor = Theme::Outline; u.rounding = 8.0f; break;
    case UIElementType::Separator: u.color = Theme::Outline; break;
    case UIElementType::InputField: u.label.clear(); u.placeholder = "Enter text..."; break;
    case UIElementType::SearchField: u.label.clear(); u.placeholder = "Search..."; break;
    case UIElementType::RadioGroup: u.items = {"Option A", "Option B", "Option C"}; u.selectedIndex = 0; break;
    case UIElementType::ProgressBar: u.label.clear(); u.sliderValue = 0.5f; break;
    case UIElementType::RangeSlider: u.sliderMin = 0; u.sliderMax = 1; u.sliderValue = .25f; u.rangeUpper = .75f; break;
    case UIElementType::Scrollbar: u.vertical = true; u.sliderMax = 100; u.sliderValue = 0; break;
    case UIElementType::Dropdown: u.items = {"Option A", "Option B", "Option C"}; u.selectedIndex = 0; break;
    case UIElementType::EnumDropdown: u.items = {"None", "First", "Second"}; u.selectedIndex = 0; break;
    case UIElementType::ListView: case UIElementType::ReorderableList: numbered("Item ", 8); break;
    case UIElementType::GridView: numbered("Item ", 9); u.gridColumns = 3; u.rowHeight = 56; break;
    case UIElementType::TreeView:
        u.items = {"Root", "Child A", "Grandchild", "Child B"}; u.itemParents = {-1, 0, 1, 0}; u.expandedItems = {0, 1}; break;
    case UIElementType::MultiColumnListView:
        u.label = "Name|Type|Value"; u.gridColumns = 3; u.sortable = true;
        u.items = {"Sword|Weapon|12", "Shield|Armor|8", "Potion|Consumable|3", "Key|Quest|1"}; break;
    case UIElementType::LogView:
        u.rowHeight = 20; u.items = {"[INFO] Game started", "[WARN] Texture missing: grass.png",
                                     "[ERROR] Script failed to load", "[INFO] Level loaded"}; break;
    case UIElementType::TabView: u.items = {"General", "Settings"}; u.selectedIndex = 0; break;
    case UIElementType::Foldout: u.open = true; break;
    case UIElementType::Toolbar: u.items = {"Select", "Move", "Rotate", "Scale"}; u.selectedIndex = 0; break;
    case UIElementType::Breadcrumb: u.items = {"Home", "Levels", "Forest"}; break;
    case UIElementType::MenuBar:
        u.items = {"File", "Edit", "View", "New", "Open", "Save", "Undo", "Redo"};
        u.itemParents = {-1, -1, -1, 0, 0, 0, 1, 1}; break;
    case UIElementType::PopupMenu: u.label = "Options"; u.items = {"Rename", "Duplicate", "Delete"}; break;
    case UIElementType::Modal: u.label = "Dialog"; u.inputText = "Are you sure you want to continue?"; u.open = false; break;
    case UIElementType::ColorPicker: u.numericValue = Theme::Accent; break;
    case UIElementType::NumericField: case UIElementType::Vector2Field:
    case UIElementType::Vector3Field: case UIElementType::Vector4Field:
        u.sliderMin = u.sliderMax = 0; u.numericStep = .1f; break; // equal bounds: unclamped
    case UIElementType::SplitView: u.sliderValue = .5f; break;
    case UIElementType::Tooltip: u.tooltip = "Tooltip text"; break;
    case UIElementType::FileBrowser: u.placeholder.clear(); break; // holds the last directory error
    default: break;
    }
}

inline int ResizeText(ImGuiInputTextCallbackData* data) {
    auto& value = *static_cast<std::string*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        value.resize(data->BufTextLen);
        data->Buf = &value[0];
    }
    return 0;
}
inline bool EditText(const char* id, std::string& text, const char* hint = "",
                     bool multiline = false, bool password = false, ImVec2 size = ImVec2(0, 0),
                     bool* submitted = nullptr) {
    if (text.capacity() < 64) text.reserve(64);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue;
    if (password) flags |= ImGuiInputTextFlags_Password;
    bool changed;
    if (multiline && !password)
        changed = ImGui::InputTextMultiline(id, &text[0], text.capacity() + 1, size, flags, ResizeText, &text);
    else
        changed = ImGui::InputTextWithHint(id, hint, &text[0], text.capacity() + 1, flags, ResizeText, &text);
    if (submitted) *submitted = changed;
    return ImGui::IsItemEdited();
}
inline SceneObject* Find(std::vector<SceneObject>& scene, int id) {
    auto it = std::find_if(scene.begin(), scene.end(), [id](const SceneObject& o) { return o.id == id; });
    return it == scene.end() ? nullptr : &*it;
}
inline void ItemState(UIElementComponent& u) {
    u.uiHovered |= ImGui::IsItemHovered();
    u.uiActive |= ImGui::IsItemActive();
    u.uiFocused |= ImGui::IsItemFocused();
}
inline bool Match(const std::string& text, const std::string& filter) {
    if (filter.empty()) return true;
    return std::search(text.begin(), text.end(), filter.begin(), filter.end(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }) != text.end();
}
inline void Select(UIElementComponent& u, int index) {
    if (u.selectedIndex != index) { u.selectedIndex = index; u.valueChanged = true; }
    u.buttonPressed = true;
}

#pragma region Palette and drawing helpers
inline ImVec4 ToIm(const glm::vec4& c) { return ImVec4(c.r, c.g, c.b, c.a); }
inline ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
inline ImVec4 Lift(const ImVec4& c, float amount) { return Mix(c, ImVec4(1, 1, 1, c.w), amount); }
inline ImVec4 WithAlpha(ImVec4 c, float a) { c.w = a; return c; }
inline float Luminance(const ImVec4& c) { return .2126f * c.x + .7152f * c.y + .0722f * c.z; }
inline ImU32 U32(const ImVec4& c) { return ImGui::GetColorU32(c); }

// Every color a control draws with, resolved once from the component. `color` is the tint:
// the accent of interactive controls and the fill of panels. Overrides with alpha 0 derive
// from it, so recoloring one field re-themes the whole control.
struct Palette {
    ImVec4 accent, background, border, text, textDim, onAccent;
};
inline Palette ResolvePalette(const UIElementComponent& u) {
    Palette p;
    p.accent = ToIm(u.fillColor.a > 0 ? u.fillColor : u.color);
    const ImVec4 opaqueAccent = WithAlpha(p.accent, 1);
    p.background = u.backgroundColor.a > 0 ? ToIm(u.backgroundColor) : Mix(ToIm(Theme::Frame), opaqueAccent, .08f);
    p.border = u.borderColor.a > 0 ? ToIm(u.borderColor) : WithAlpha(Mix(p.background, opaqueAccent, .32f), .9f);
    p.text = u.textColor.a > 0 ? ToIm(u.textColor) : ToIm(Theme::Text);
    p.textDim = Mix(p.background, p.text, .55f);
    // Fills must read against their track, whatever tint a scene saved.
    for (int i = 0; i < 6 && std::abs(Luminance(p.accent) - Luminance(p.background)) < .22f; ++i)
        p.accent = Lift(p.accent, .25f);
    p.onAccent = Luminance(p.accent) > .55f ? ImVec4(.08f, .07f, .11f, 1) : ImVec4(1, 1, 1, 1);
    return p;
}

// Pops what it pushed, in the right amounts, on every exit path.
class StyleScope {
    int colors = 0, vars = 0;
public:
    StyleScope() = default;
    StyleScope(const StyleScope&) = delete;
    StyleScope& operator=(const StyleScope&) = delete;
    ~StyleScope() { ImGui::PopStyleVar(vars); ImGui::PopStyleColor(colors); }
    void Color(ImGuiCol idx, const ImVec4& c) { ImGui::PushStyleColor(idx, c); ++colors; }
    void Var(ImGuiStyleVar idx, float v) { ImGui::PushStyleVar(idx, v); ++vars; }
    void Var(ImGuiStyleVar idx, const ImVec2& v) { ImGui::PushStyleVar(idx, v); ++vars; }
};
inline void ApplyPalette(StyleScope& s, const Palette& p, const UIElementComponent& u) {
    const ImVec4& bg = p.background;
    s.Color(ImGuiCol_Text, p.text); s.Color(ImGuiCol_TextDisabled, p.textDim);
    s.Color(ImGuiCol_FrameBg, bg); s.Color(ImGuiCol_FrameBgHovered, Lift(bg, .05f)); s.Color(ImGuiCol_FrameBgActive, Lift(bg, .08f));
    s.Color(ImGuiCol_Button, Lift(bg, .07f)); s.Color(ImGuiCol_ButtonHovered, Lift(bg, .13f)); s.Color(ImGuiCol_ButtonActive, Mix(bg, p.accent, .4f));
    s.Color(ImGuiCol_Header, Mix(bg, p.accent, .55f)); s.Color(ImGuiCol_HeaderHovered, Mix(bg, p.accent, .22f));
    s.Color(ImGuiCol_HeaderActive, Mix(bg, p.accent, .7f));
    s.Color(ImGuiCol_CheckMark, p.accent); s.Color(ImGuiCol_SliderGrab, p.accent); s.Color(ImGuiCol_SliderGrabActive, Lift(p.accent, .15f));
    s.Color(ImGuiCol_PlotHistogram, p.accent); s.Color(ImGuiCol_Border, p.border); s.Color(ImGuiCol_Separator, p.border);
    s.Color(ImGuiCol_ChildBg, bg); s.Color(ImGuiCol_PopupBg, WithAlpha(Lift(bg, .04f), 1));
    s.Color(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0)); s.Color(ImGuiCol_ScrollbarGrab, Lift(bg, .14f));
    s.Color(ImGuiCol_ScrollbarGrabHovered, Lift(bg, .22f)); s.Color(ImGuiCol_ScrollbarGrabActive, p.accent);
    s.Color(ImGuiCol_Tab, bg); s.Color(ImGuiCol_TabHovered, Mix(bg, p.accent, .3f)); s.Color(ImGuiCol_TabSelected, Lift(bg, .09f));
    s.Color(ImGuiCol_TabSelectedOverline, p.accent); s.Color(ImGuiCol_TabDimmed, bg); s.Color(ImGuiCol_TabDimmedSelected, Lift(bg, .09f));
    s.Color(ImGuiCol_TabDimmedSelectedOverline, p.accent);
    s.Color(ImGuiCol_TableHeaderBg, Lift(bg, .06f)); s.Color(ImGuiCol_TableBorderStrong, p.border);
    s.Color(ImGuiCol_TableBorderLight, WithAlpha(p.border, p.border.w * .6f)); s.Color(ImGuiCol_TableRowBgAlt, ImVec4(1, 1, 1, .025f));
    s.Color(ImGuiCol_TextSelectedBg, WithAlpha(p.accent, .35f)); s.Color(ImGuiCol_NavCursor, p.accent);
    s.Color(ImGuiCol_InputTextCursor, p.accent); s.Color(ImGuiCol_DragDropTarget, p.accent); s.Color(ImGuiCol_TreeLines, p.border);
    s.Color(ImGuiCol_ModalWindowDimBg, u.dimBackground ? ImVec4(.04f, .03f, .06f, .55f) : ImVec4(0, 0, 0, 0));
    const float rounding = std::max(0.0f, u.rounding);
    s.Var(ImGuiStyleVar_FrameRounding, rounding); s.Var(ImGuiStyleVar_FrameBorderSize, std::max(0.0f, u.borderWidth));
    s.Var(ImGuiStyleVar_ChildRounding, rounding); s.Var(ImGuiStyleVar_ChildBorderSize, std::max(0.0f, u.borderWidth));
    s.Var(ImGuiStyleVar_GrabRounding, rounding); s.Var(ImGuiStyleVar_PopupRounding, std::min(rounding, 8.0f));
    s.Var(ImGuiStyleVar_ScrollbarSize, 10.0f); s.Var(ImGuiStyleVar_ScrollbarRounding, 5.0f);
}
// Single-line frames (inputs, combos, drags) fill their rect instead of hugging its top edge.
inline ImVec2 FitFramePadding(float height) {
    return ImVec2(std::max(6.0f, ImGui::GetStyle().FramePadding.x), std::max(2.0f, std::floor((height - ImGui::GetFontSize()) * .5f)));
}
inline ImGuiShadeState ShadeState(bool hovered, bool held, bool selected = false) {
    return held ? ImGuiShadeState_Active : selected ? ImGuiShadeState_Selected : hovered ? ImGuiShadeState_Hovered : ImGuiShadeState_Normal;
}
inline void Chevron(ImDrawList* dl, ImVec2 c, float half, ImGuiDir dir, ImU32 col, float thickness = 1.5f) {
    ImVec2 a, b, tip;
    switch (dir) {
    case ImGuiDir_Down: a = ImVec2(c.x - half, c.y - half * .5f); b = ImVec2(c.x + half, c.y - half * .5f); tip = ImVec2(c.x, c.y + half * .5f); break;
    case ImGuiDir_Up: a = ImVec2(c.x - half, c.y + half * .5f); b = ImVec2(c.x + half, c.y + half * .5f); tip = ImVec2(c.x, c.y - half * .5f); break;
    case ImGuiDir_Left: a = ImVec2(c.x + half * .5f, c.y - half); b = ImVec2(c.x + half * .5f, c.y + half); tip = ImVec2(c.x - half * .5f, c.y); break;
    default: a = ImVec2(c.x - half * .5f, c.y - half); b = ImVec2(c.x - half * .5f, c.y + half); tip = ImVec2(c.x + half * .5f, c.y); break;
    }
    dl->PathLineTo(a); dl->PathLineTo(tip); dl->PathLineTo(b); dl->PathStroke(col, 0, thickness);
}
inline void CenteredText(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, const char* text, const char* end = nullptr) {
    const ImVec2 ts = ImGui::CalcTextSize(text, end);
    dl->AddText(ImVec2(std::floor(min.x + (max.x - min.x - ts.x) * .5f), std::floor(min.y + (max.y - min.y - ts.y) * .5f)), col, text, end);
}
// Clickable face of a dropdown or popup trigger: text on the left, chevron on the right.
inline bool DropdownFace(const char* id, const char* text, bool dim, ImVec2 min, ImVec2 size,
                         const Palette& pal, float rounding, bool open, ImGuiShadeClass shade = ImGuiShadeClass_Frame) {
    ImGui::SetCursorScreenPos(min);
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const ImVec4 face = shade == ImGuiShadeClass_Button ? Lift(pal.background, hovered ? .13f : .07f) : Lift(pal.background, hovered ? .05f : 0);
    ImGui::ShadeRect(dl, min, max, U32(face), shade, ShadeState(hovered, held, open), rounding);
    if (ImGui::GetStyle().FrameBorderSize > 0) dl->AddRect(min, max, U32(open ? pal.accent : pal.border), rounding, 0, ImGui::GetStyle().FrameBorderSize);
    const float arrow = std::min(size.y, 28.0f);
    const ImVec2 textMax(max.x - arrow, max.y);
    ImGui::PushClipRect(min, textMax, true);
    const float pad = FitFramePadding(size.y).x;
    dl->AddText(ImVec2(min.x + pad, std::floor(min.y + (size.y - ImGui::GetFontSize()) * .5f)), U32(dim ? pal.textDim : pal.text), text);
    ImGui::PopClipRect();
    Chevron(dl, ImVec2(max.x - arrow * .5f, min.y + size.y * .5f), 4.0f, open ? ImGuiDir_Up : ImGuiDir_Down, U32(hovered ? pal.text : pal.textDim));
    return pressed;
}
inline void DashedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float dash = 5.0f) {
    for (float x = min.x; x < max.x; x += dash * 2) {
        dl->AddLine(ImVec2(x, min.y), ImVec2(std::min(x + dash, max.x), min.y), col);
        dl->AddLine(ImVec2(x, max.y - 1), ImVec2(std::min(x + dash, max.x), max.y - 1), col);
    }
    for (float y = min.y; y < max.y; y += dash * 2) {
        dl->AddLine(ImVec2(min.x, y), ImVec2(min.x, std::min(y + dash, max.y)), col);
        dl->AddLine(ImVec2(max.x - 1, y), ImVec2(max.x - 1, std::min(y + dash, max.y)), col);
    }
}
// Arrow keys adjust a focused custom slider instead of moving navigation away from it.
inline float KeyboardStep(ImGuiID id) {
    if (!ImGui::IsItemFocused()) return 0;
    float direction = 0;
    for (ImGuiKey key : {ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight}) {
        ImGui::SetKeyOwner(key, id);
        if (ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, id))
            direction += (key == ImGuiKey_LeftArrow || key == ImGuiKey_GamepadDpadLeft) ? -1.0f : 1.0f;
    }
    return direction;
}
#pragma endregion

inline void MenuItems(UIElementComponent& u, int parent = -1, int depth = 0) {
    if (depth > 32) return;
    for (int i = 0; i < static_cast<int>(u.items.size()); ++i) {
        int p = i < static_cast<int>(u.itemParents.size()) ? u.itemParents[i] : -1;
        if (p != parent) continue;
        bool hasChildren = std::find(u.itemParents.begin(), u.itemParents.end(), i) != u.itemParents.end();
        ImGui::PushID(i);
        if (hasChildren) {
            if (ImGui::BeginMenu(u.items[i].c_str())) { MenuItems(u, i, depth + 1); ImGui::EndMenu(); }
        } else if (ImGui::MenuItem(u.items[i].c_str())) Select(u, i);
        ImGui::PopID();
    }
}
inline void TreeItems(UIElementComponent& u, int parent = -1, int depth = 0) {
    if (depth > 64) return;
    for (int i = u.itemFirstChild[parent + 1]; i >= 0; i = u.itemNextSibling[i]) {
        bool hasChildren = u.itemFirstChild[i + 1] >= 0;
        ImGui::PushID(i);
        auto expanded = std::find(u.expandedItems.begin(), u.expandedItems.end(), i);
        ImGui::SetNextItemOpen(expanded != u.expandedItems.end(), ImGuiCond_Always);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_DrawLinesToNodes;
        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
        if (u.selectedIndex == i) flags |= ImGuiTreeNodeFlags_Selected;
        bool open = ImGui::TreeNodeEx("##node", flags, "%s", u.items[i].c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Select(u, i);
        if (ImGui::IsItemToggledOpen()) {
            if (open && expanded == u.expandedItems.end()) u.expandedItems.push_back(i);
            else if (!open && expanded != u.expandedItems.end()) u.expandedItems.erase(expanded);
        }
        ItemState(u);
        if (open) { if (hasChildren) TreeItems(u, i, depth + 1); ImGui::TreePop(); }
        ImGui::PopID();
    }
}
// Log lines tagged as warnings/errors are tinted so they stand out in a long log.
inline ImVec4 LogLineColor(const std::string& line, const Palette& pal) {
    char head[16] = {};
    for (size_t i = 0; i < sizeof(head) - 1 && i < line.size(); ++i) head[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
    if (std::strstr(head, "err") || std::strstr(head, "fatal")) return ImVec4(.95f, .45f, .45f, pal.text.w);
    if (std::strstr(head, "warn")) return ImVec4(.92f, .76f, .38f, pal.text.w);
    return pal.text;
}
inline void Collection(UIElementComponent& u, ImVec2 size, const Palette& pal) {
    const ImGuiID owner = ImGui::GetID("##collection");
    StyleScope local;
    local.Var(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));
    local.Var(ImGuiStyleVar_ItemSpacing, ImVec2(6, 2));
    local.Var(ImGuiStyleVar_SelectableTextAlign, u.type == UIElementType::GridView ? ImVec2(.5f, .5f) : ImVec2(0, .5f));
    if (u.type == UIElementType::TreeView) local.Var(ImGuiStyleVar_FramePadding, ImVec2(4, std::max(1.0f, (u.rowHeight - ImGui::GetFontSize()) * .5f)));
    bool visible = ImGui::BeginChild("##collection", size, ImGuiChildFlags_Borders);
    if (visible) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (u.sortable && u.type != UIElementType::TreeView && ImGui::SmallButton("Sort A-Z")) {
            std::string selected = u.selectedIndex >= 0 && u.selectedIndex < static_cast<int>(u.items.size()) ? u.items[u.selectedIndex] : std::string{};
            std::stable_sort(u.items.begin(), u.items.end());
            auto it = std::find(u.items.begin(), u.items.end(), selected);
            u.selectedIndex = it == u.items.end() ? -1 : static_cast<int>(it - u.items.begin());
            u.valueChanged = true;
        }
        const float height = std::max(1.0f, u.rowHeight);
        if (u.items.empty()) {
            const ImVec2 wmin = ImGui::GetWindowPos(), wsize = ImGui::GetWindowSize();
            CenteredText(dl, wmin, ImVec2(wmin.x + wsize.x, wmin.y + wsize.y), U32(pal.textDim), "No items");
        } else if (u.type == UIElementType::TreeView) {
            u.itemFirstChild.assign(u.items.size() + 1, -1); u.itemNextSibling.assign(u.items.size(), -1);
            for (int i = static_cast<int>(u.items.size()) - 1; i >= 0; --i) {
                int parent = i < static_cast<int>(u.itemParents.size()) ? u.itemParents[i] : -1;
                if (parent < -1 || parent >= static_cast<int>(u.items.size()) || parent == i) parent = -1;
                u.itemNextSibling[i] = u.itemFirstChild[parent + 1]; u.itemFirstChild[parent + 1] = i;
            }
            TreeItems(u);
        }
        else {
            // Keep row IDs in source order: filtering and virtualization do not change selection IDs.
            auto& visibleItems = u.visibleItemIndices;
            visibleItems.clear();
            const bool filtered = !u.filterText.empty();
            if (filtered) {
                visibleItems.reserve(u.items.size());
                for (int i = 0; i < static_cast<int>(u.items.size()); ++i)
                    if (Match(u.items[i], u.filterText)) visibleItems.push_back(i);
            }
            const bool log = u.type == UIElementType::LogView;
            const bool reorder = u.type == UIElementType::ReorderableList;
            const bool followTail = log && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
            const int count = static_cast<int>(filtered ? visibleItems.size() : u.items.size());
            const int columns = u.type == UIElementType::GridView ? std::clamp(u.gridColumns, 1, 1024) : 1;
            const int rows = (count + columns - 1) / columns;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float cellWidth = columns > 1 ? std::max(1.0f, (ImGui::GetContentRegionAvail().x - gap * (columns - 1)) / columns) : 0;
            const float handleWidth = reorder ? 18.0f : 0.0f;
            ImGuiListClipper clipper;
            clipper.Begin(rows, height + ImGui::GetStyle().ItemSpacing.y);
            if (!u.virtualize) clipper.IncludeItemsByIndex(0, rows);
            while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int col = 0; col < columns; ++col) {
                    int item = row * columns + col;
                    if (item >= count) break;
                    int index = filtered ? visibleItems[item] : item;
                    ImGui::PushID(index);
                    if (col) ImGui::SameLine(0, gap);
                    const bool selected = u.selectedIndex == index;
                    if (columns > 1) {
                        const ImVec2 cell = ImGui::GetCursorScreenPos();
                        dl->AddRectFilled(cell, ImVec2(cell.x + cellWidth, cell.y + height), U32(Lift(pal.background, .05f)), std::min(u.rounding, 6.0f));
                    }
                    const bool custom = log || reorder; // text drawn by hand: tinted or offset by a handle
                    if (ImGui::Selectable(custom ? "##row" : u.items[index].c_str(), selected, 0, ImVec2(cellWidth, height))) Select(u, index);
                    ItemState(u);
                    const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
                    if (custom) {
                        const float textY = std::floor(rmin.y + (height - ImGui::GetFontSize()) * .5f);
                        dl->AddText(ImVec2(rmin.x + 4 + handleWidth, textY), U32(log ? LogLineColor(u.items[index], pal) : pal.text), u.items[index].c_str());
                    }
                    if (reorder) {
                        const ImU32 grip = U32(ImGui::IsItemHovered() || selected ? pal.text : pal.textDim);
                        const float cy = (rmin.y + rmax.y) * .5f;
                        for (int line = -1; line <= 1; ++line)
                            dl->AddLine(ImVec2(rmin.x + 5, cy + line * 3.5f), ImVec2(rmin.x + 14, cy + line * 3.5f), grip, 1.2f);
                        if (ImGui::BeginDragDropSource()) {
                            const int payloadData[2] = {static_cast<int>(owner), index};
                            ImGui::SetDragDropPayload("MODU_UI_ROW", payloadData, sizeof(payloadData));
                            ImGui::TextUnformatted(u.items[index].c_str());
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget()) {
                            const ImGuiDragDropFlags peek = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODU_UI_ROW", peek)) {
                                if (payload->DataSize == sizeof(int) * 2 &&
                                    static_cast<const int*>(payload->Data)[0] == static_cast<int>(owner)) {
                                    int source = static_cast<const int*>(payload->Data)[1];
                                    // Insertion marker on the edge the row will land on.
                                    const float edge = source < index ? rmax.y : rmin.y;
                                    dl->AddLine(ImVec2(rmin.x, edge), ImVec2(rmax.x, edge), U32(pal.accent), 2.0f);
                                    if (payload->IsDelivery() && source >= 0 && source < static_cast<int>(u.items.size()) && source != index) {
                                        auto text = std::move(u.items[source]);
                                        u.items.erase(u.items.begin() + source);
                                        u.items.insert(u.items.begin() + index, std::move(text));
                                        u.selectedIndex = index; u.valueChanged = true;
                                    }
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                    }
                    ImGui::PopID();
                }
            }
            // A log that was showing its newest line keeps showing it as lines arrive.
            if (followTail) {
                ImGuiStorage* storage = ImGui::GetStateStorage();
                const ImGuiID countKey = ImGui::GetID("##logCount");
                if (storage->GetInt(countKey, -1) != count) ImGui::SetScrollHereY(1.0f);
                storage->SetInt(countKey, count);
            }
        }
        u.scrollOffset = glm::vec2(ImGui::GetScrollX(), ImGui::GetScrollY());
    }
    ImGui::EndChild();
}

#pragma region Custom-drawn controls
inline void RangeSlider(UIElementComponent& u, ImVec2 min, ImVec2 max, const Palette& pal) {
    const float lo = std::min(u.sliderMin, u.sliderMax), hi = std::max(u.sliderMin, u.sliderMax), range = hi - lo;
    u.sliderValue = std::clamp(u.sliderValue, lo, hi); u.rangeUpper = std::clamp(u.rangeUpper, u.sliderValue, hi);
    const ImVec2 size(max.x - min.x, max.y - min.y);
    const float fontSize = ImGui::GetFontSize();
    const float knob = std::clamp(size.y * .3f, 5.0f, 9.0f);
    const bool showValues = size.y >= knob * 2 + fontSize + 4;
    const float cy = showValues ? min.y + knob + 2 : min.y + size.y * .5f;
    const float x0 = min.x + knob + 1, x1 = max.x - knob - 1, span = std::max(1.0f, x1 - x0);
    auto toX = [&](float v) { return range > 0 ? x0 + (v - lo) / range * span : x0; };
    auto fromX = [&](float x) { return lo + std::clamp((x - x0) / span, 0.0f, 1.0f) * range; };

    ImGui::InvisibleButton("##range", size, ImGuiButtonFlags_EnableNav);
    const ImGuiID id = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    int& handle = *ImGui::GetStateStorage()->GetIntRef(id, 0); // 0 lower, 1 upper
    const float mouseX = ImGui::GetIO().MousePos.x;
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float da = std::abs(mouseX - toX(u.sliderValue)), db = std::abs(mouseX - toX(u.rangeUpper));
        handle = (db < da || (da == db && mouseX > toX(u.rangeUpper))) ? 1 : 0;
    }
    float* target = handle ? &u.rangeUpper : &u.sliderValue;
    float next = *target;
    if (held && ImGui::IsMouseDown(ImGuiMouseButton_Left)) next = fromX(mouseX);
    // Keyboard/gamepad: arrows move the lower handle, Shift+arrows the upper one.
    if (const float step = KeyboardStep(id)) {
        handle = ImGui::GetIO().KeyShift ? 1 : 0;
        target = handle ? &u.rangeUpper : &u.sliderValue;
        next = *target + step * std::max(range / 100.0f, 1e-4f) * (ImGui::GetIO().KeyCtrl ? 10.0f : 1.0f);
    }
    next = handle ? std::clamp(next, u.sliderValue, hi) : std::clamp(next, lo, u.rangeUpper);
    if (next != *target) { *target = next; u.valueChanged = true; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float track = std::max(4.0f, knob * .7f);
    const ImVec2 tmin(x0 - knob * .5f, cy - track * .5f), tmax(x1 + knob * .5f, cy + track * .5f);
    ImGui::ShadeRect(dl, tmin, tmax, U32(Lift(pal.background, .1f)), ImGuiShadeClass_Frame, ShadeState(hovered, false), track * .5f);
    const float xa = toX(u.sliderValue), xb = toX(u.rangeUpper);
    dl->AddRectFilled(ImVec2(xa, tmin.y), ImVec2(std::max(xa + 1, xb), tmax.y), U32(pal.accent), track * .5f);
    for (int i = 0; i < 2; ++i) {
        const ImVec2 c(i ? xb : xa, cy);
        const bool hot = held && handle == i;
        const bool nearHandle = hovered && std::abs(mouseX - c.x) <= knob * 1.5f;
        if (hot || nearHandle) dl->AddCircleFilled(c, knob + 4, U32(WithAlpha(pal.accent, hot ? .3f : .18f)));
        dl->AddCircleFilled(c, knob, U32(Mix(pal.accent, ImVec4(1, 1, 1, 1), .82f)));
        dl->AddCircle(c, knob, U32(pal.accent), 0, 2.0f);
    }
    if (showValues) {
        const char* fmt = range >= 20 ? "%.0f" : "%.2f";
        char a[32], b[32];
        std::snprintf(a, sizeof a, fmt, u.sliderValue); std::snprintf(b, sizeof b, fmt, u.rangeUpper);
        const float ty = cy + knob + 3;
        dl->AddText(ImVec2(min.x, ty), U32(pal.textDim), a);
        dl->AddText(ImVec2(max.x - ImGui::CalcTextSize(b).x, ty), U32(pal.textDim), b);
    }
}
inline void ProgressBar(UIElementComponent& u, ImVec2 min, ImVec2 max, const Palette& pal) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(max.x - min.x, max.y - min.y);
    const float r = std::min(std::max(0.0f, u.rounding), size.y * .5f);
    ImGui::ShadeRect(dl, min, max, U32(pal.background), ImGuiShadeClass_Frame, ImGuiShadeState_Normal, r);
    if (u.borderWidth > 0) dl->AddRect(min, max, U32(pal.border), r, 0, u.borderWidth);
    const float range = u.sliderMax - u.sliderMin;
    const float fraction = range > 0 ? std::clamp((u.sliderValue - u.sliderMin) / range, 0.0f, 1.0f) : 0.0f;
    const float inset = std::min(2.0f, size.y * .15f);
    const ImVec2 fmin(min.x + inset, min.y + inset), fmax(max.x - inset, max.y - inset);
    const float fr = std::max(0.0f, r - inset);
    if (u.indeterminate) {
        // A segment sweeps across the track with ease-in-out, then re-enters from the left.
        const float t = static_cast<float>(std::fmod(ImGui::GetTime() * .8, 1.0));
        const float eased = t * t * (3 - 2 * t);
        const float seg = (fmax.x - fmin.x) * .35f;
        const float x = fmin.x - seg + (fmax.x - fmin.x + seg) * eased;
        ImGui::PushClipRect(fmin, fmax, true);
        dl->AddRectFilled(ImVec2(x, fmin.y), ImVec2(x + seg, fmax.y), U32(pal.accent), fr);
        ImGui::PopClipRect();
    } else if (fraction > 0) {
        const float w = (fmax.x - fmin.x) * fraction;
        ImGui::ShadeRect(dl, fmin, ImVec2(fmin.x + w, fmax.y), U32(pal.accent), ImGuiShadeClass_Grab, ImGuiShadeState_Normal, std::min(fr, w * .5f));
    }
    char percent[16];
    const char* text = u.label.c_str();
    if (u.label.empty()) {
        if (u.indeterminate) return;
        std::snprintf(percent, sizeof percent, "%d%%", static_cast<int>(std::round(fraction * 100)));
        text = percent;
    }
    if (size.y < ImGui::GetFontSize()) return;
    CenteredText(dl, ImVec2(min.x + 1, min.y + 1), ImVec2(max.x + 1, max.y + 1), IM_COL32(0, 0, 0, 110), text);
    CenteredText(dl, min, max, U32(pal.text), text);
}
inline void Scrollbar(UIElementComponent& u, std::vector<SceneObject>& scene, ImVec2 min, ImVec2 max, const Palette& pal) {
    SceneObject* target = Find(scene, u.linkedObjectId);
    const int axis = u.vertical ? 1 : 0;
    const ImVec2 size(max.x - min.x, max.y - min.y);
    // Linked: the scroll view's own viewport/content extents. Standalone: sliderMax is the scroll range.
    const float viewport = target ? std::max(1.0f, target->ui.size[axis]) : std::max(1.0f, (&size.x)[axis]);
    const float limit = target ? std::max(0.0f, target->ui.contentSize[axis] - target->ui.size[axis]) : std::max(0.0f, u.sliderMax);
    float& value = target ? target->ui.scrollOffset[axis] : u.sliderValue;
    value = std::clamp(value, 0.0f, limit);
    const float trackStart = (&min.x)[axis] + 2, trackLen = std::max(1.0f, (&size.x)[axis] - 4);
    const float thumbLen = limit > 0 ? std::clamp(trackLen * viewport / (viewport + limit), std::min(trackLen, 16.0f), trackLen) : trackLen;
    const float travel = std::max(0.0f, trackLen - thumbLen);
    auto thumbStart = [&] { return trackStart + (limit > 0 ? value / limit * travel : 0); };

    ImGui::InvisibleButton("##scroll", size, ImGuiButtonFlags_EnableNav);
    const ImGuiID id = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    float& grab = *ImGui::GetStateStorage()->GetFloatRef(id, -1.0f); // mouse offset into the thumb, -1 = paging
    const float mouse = (&ImGui::GetIO().MousePos.x)[axis];
    const float before = value;
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float start = thumbStart();
        if (mouse >= start && mouse <= start + thumbLen) grab = mouse - start;
        else { grab = -1; value += (mouse < start ? -1.0f : 1.0f) * viewport * .9f; }
    }
    if (held && grab >= 0 && travel > 0) value = (mouse - grab - trackStart) / travel * limit;
    if (hovered) value -= (axis ? ImGui::GetIO().MouseWheel : ImGui::GetIO().MouseWheelH + ImGui::GetIO().MouseWheel) * std::max(1.0f, u.rowHeight) * 2;
    value += KeyboardStep(id) * std::max(1.0f, u.rowHeight);
    value = std::clamp(value, 0.0f, limit);
    u.valueChanged = value != before;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float thickness = (&size.x)[1 - axis];
    ImGui::ShadeRect(dl, min, max, U32(pal.background), ImGuiShadeClass_ScrollbarBg, ImGuiShadeState_Normal, thickness * .5f);
    const float start = thumbStart();
    ImVec2 tmin = min, tmax = max;
    (&tmin.x)[axis] = start; (&tmax.x)[axis] = start + thumbLen;
    (&tmin.x)[1 - axis] += 2; (&tmax.x)[1 - axis] -= 2;
    const ImVec4 thumb = held ? pal.accent : hovered ? Mix(pal.background, pal.accent, .6f) : Lift(pal.background, .2f);
    ImGui::ShadeRect(dl, tmin, tmax, U32(thumb), ImGuiShadeClass_Grab, ShadeState(hovered, held), (thickness - 4) * .5f);
}
inline void Foldout(UIElementComponent& u, ImVec2 min, ImVec2 max, const Palette& pal) {
    const ImVec2 size(max.x - min.x, max.y - min.y);
    if (ImGui::InvisibleButton("##foldout", size, ImGuiButtonFlags_EnableNav)) { u.open = !u.open; u.valueChanged = true; }
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = std::max(0.0f, u.rounding);
    ImGui::ShadeRect(dl, min, max, U32(Lift(pal.background, hovered ? .1f : .05f)), ImGuiShadeClass_Header, ShadeState(hovered, held), r);
    if (u.borderWidth > 0) dl->AddRect(min, max, U32(pal.border), r, 0, u.borderWidth);
    const float pad = FitFramePadding(size.y).x;
    Chevron(dl, ImVec2(min.x + pad + 5, min.y + size.y * .5f), 4.5f, u.open ? ImGuiDir_Down : ImGuiDir_Right, U32(u.open ? pal.accent : pal.text), 1.8f);
    dl->AddText(ImVec2(min.x + pad + 18, std::floor(min.y + (size.y - ImGui::GetFontSize()) * .5f)), U32(pal.text), u.label.c_str());
}
// Segmented control: one joined strip, the selected segment filled with the accent.
inline void Toolbar(UIElementComponent& u, ImVec2 min, ImVec2 max, const Palette& pal) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(max.x - min.x, max.y - min.y);
    const float r = std::max(0.0f, u.rounding);
    ImGui::ShadeRect(dl, min, max, U32(pal.background), ImGuiShadeClass_Frame, ImGuiShadeState_Normal, r);
    if (u.borderWidth > 0) dl->AddRect(min, max, U32(pal.border), r, 0, u.borderWidth);
    const int n = static_cast<int>(u.items.size());
    if (!n) return;
    const float seg = size.x / n;
    for (int i = 0; i < n; ++i) {
        const ImVec2 smin(std::floor(min.x + seg * i), min.y), smax(std::floor(min.x + seg * (i + 1)), max.y);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(smin);
        if (ImGui::InvisibleButton("##segment", ImVec2(smax.x - smin.x, size.y), ImGuiButtonFlags_EnableNav)) Select(u, i);
        ItemState(u);
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive(), selected = u.selectedIndex == i;
        const ImVec2 imin(smin.x + 2, smin.y + 2), imax(smax.x - 2, smax.y - 2);
        if (selected) ImGui::ShadeRect(dl, imin, imax, U32(held ? Lift(pal.accent, .1f) : pal.accent), ImGuiShadeClass_Button, ImGuiShadeState_Selected, std::max(0.0f, r - 2));
        else if (hovered) dl->AddRectFilled(imin, imax, U32(Lift(pal.background, held ? .14f : .08f)), std::max(0.0f, r - 2));
        if (i > 0 && !selected && u.selectedIndex != i - 1)
            dl->AddLine(ImVec2(smin.x, min.y + size.y * .25f), ImVec2(smin.x, max.y - size.y * .25f), U32(pal.border));
        ImGui::PushClipRect(imin, imax, true);
        CenteredText(dl, smin, smax, U32(selected ? pal.onAccent : pal.text), u.items[i].c_str());
        ImGui::PopClipRect();
        ImGui::PopID();
    }
}
// Path segments separated by chevrons; leading segments collapse into "..." when space runs out.
inline void Breadcrumb(UIElementComponent& u, ImVec2 min, ImVec2 max, const Palette& pal) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = static_cast<int>(u.items.size());
    const float sep = 16.0f, pad = 4.0f, cy = (min.y + max.y) * .5f, textY = std::floor(cy - ImGui::GetFontSize() * .5f);
    const float ellipsis = ImGui::CalcTextSize("...").x + sep;
    float total = 0;
    for (const auto& item : u.items) total += ImGui::CalcTextSize(item.c_str()).x + pad * 2 + sep;
    total -= sep;
    int first = 0;
    while (first < n - 1 && total + (first ? ellipsis : 0) > max.x - min.x)
        total -= ImGui::CalcTextSize(u.items[first++].c_str()).x + pad * 2 + sep;
    float x = min.x;
    if (first) {
        dl->AddText(ImVec2(x + pad, textY), U32(pal.textDim), "...");
        x += ImGui::CalcTextSize("...").x + pad;
        Chevron(dl, ImVec2(x + sep * .5f, cy), 3.5f, ImGuiDir_Right, U32(pal.textDim));
        x += sep;
    }
    for (int i = first; i < n; ++i) {
        const float w = ImGui::CalcTextSize(u.items[i].c_str()).x + pad * 2;
        const bool last = i == n - 1;
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(x, min.y));
        if (ImGui::InvisibleButton("##crumb", ImVec2(w, max.y - min.y), ImGuiButtonFlags_EnableNav)) Select(u, i);
        ItemState(u);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) dl->AddRectFilled(ImVec2(x, min.y + 2), ImVec2(x + w, max.y - 2), U32(Lift(pal.background, .08f)), 4.0f);
        const ImVec4 col = hovered ? Lift(pal.accent, .2f) : (last || u.selectedIndex == i) ? pal.text : pal.textDim;
        dl->AddText(ImVec2(x + pad, textY), U32(col), u.items[i].c_str());
        ImGui::PopID();
        x += w;
        if (!last) { Chevron(dl, ImVec2(x + sep * .5f, cy), 3.5f, ImGuiDir_Right, U32(pal.textDim)); x += sep; }
    }
}
inline void MenuBar(UIElementComponent& u, ImVec2 min, ImVec2 max, const Palette& pal) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = std::max(0.0f, u.rounding);
    ImGui::ShadeRect(dl, min, max, U32(Lift(pal.background, .04f)), ImGuiShadeClass_MenuBar, ImGuiShadeState_Normal, r);
    if (u.borderWidth > 0) dl->AddRect(min, max, U32(pal.border), r, 0, u.borderWidth);
    auto isRoot = [&](int i) { return i >= static_cast<int>(u.itemParents.size()) || u.itemParents[i] < 0; };
    bool anyOpen = false;
    for (int i = 0; i < static_cast<int>(u.items.size()); ++i) {
        if (!isRoot(i)) continue;
        ImGui::PushID(i); anyOpen |= ImGui::IsPopupOpen("##menu"); ImGui::PopID();
    }
    float x = min.x + 4;
    for (int i = 0; i < static_cast<int>(u.items.size()); ++i) {
        if (!isRoot(i)) continue;
        const float w = ImGui::CalcTextSize(u.items[i].c_str()).x + 18;
        const ImVec2 imin(x, min.y + 3), imax(x + w, max.y - 3);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(imin);
        const bool pressed = ImGui::InvisibleButton("##title", ImVec2(w, imax.y - imin.y), ImGuiButtonFlags_EnableNav);
        ItemState(u);
        const bool open = ImGui::IsPopupOpen("##menu");
        // Classic menu bar: with one menu open, hovering another title switches to it.
        const bool switchTo = anyOpen && !open && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
        if (pressed || switchTo) ImGui::OpenPopup("##menu");
        if (open || ImGui::IsItemHovered())
            dl->AddRectFilled(imin, imax, U32(open ? Mix(pal.background, pal.accent, .45f) : Lift(pal.background, .1f)), std::min(r, 5.0f));
        CenteredText(dl, imin, imax, U32(pal.text), u.items[i].c_str());
        ImGui::SetNextWindowPos(ImVec2(imin.x, max.y + 2));
        if (ImGui::BeginPopup("##menu")) { MenuItems(u, i); ImGui::EndPopup(); }
        ImGui::PopID();
        x += w + 2;
    }
}
// The handle between the two panes; the panes themselves are positioned by UILayout.
inline void SplitHandle(UIElementComponent& u, ImVec2 min, ImVec2 max, const Palette& pal) {
    const int axis = u.vertical ? 1 : 0;
    const ImVec2 size(max.x - min.x, max.y - min.y);
    const float extent = (&size.x)[axis];
    const float authored = u.size[axis] > 1 ? u.size[axis] : extent;
    const float thickness = UILayout::kSplitterThickness * extent / authored;
    const float fraction = std::clamp(u.sliderValue, .05f, .95f);
    const float pos = (&min.x)[axis] + (extent - thickness) * fraction;
    ImVec2 hmin = min, hmax = max;
    (&hmin.x)[axis] = pos; (&hmax.x)[axis] = pos + thickness;
    ImGui::SetCursorScreenPos(hmin);
    ImGui::InvisibleButton("##splitter", ImVec2(hmax.x - hmin.x, hmax.y - hmin.y));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    if (hovered || held) ImGui::SetMouseCursor(axis ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
    if (held && extent > thickness) {
        const float mouse = (&ImGui::GetIO().MousePos.x)[axis];
        const float next = std::clamp((mouse - (&min.x)[axis] - thickness * .5f) / (extent - thickness), .05f, .95f);
        if (next != u.sliderValue) { u.sliderValue = next; u.valueChanged = true; }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 col = held ? pal.accent : hovered ? Mix(pal.border, pal.accent, .6f) : pal.border;
    const float mid = pos + thickness * .5f;
    ImVec2 lmin = hmin, lmax = hmax;
    (&lmin.x)[axis] = mid - 1; (&lmax.x)[axis] = mid + 1;
    dl->AddRectFilled(lmin, lmax, U32(col), 1.0f);
    // Grip dots in the middle of the handle.
    const float center = ((&min.x)[1 - axis] + (&max.x)[1 - axis]) * .5f;
    for (int d = -1; d <= 1; ++d) {
        ImVec2 dot; (&dot.x)[axis] = mid; (&dot.x)[1 - axis] = center + d * 6.0f;
        dl->AddCircleFilled(dot, 2.0f, U32(held || hovered ? pal.accent : Lift(pal.border, .25f)));
    }
}
inline void VectorField(UIElementComponent& u, int count, ImVec2 size) {
    static const ImVec4 axes[4] = {ImVec4(.86f, .34f, .36f, 1), ImVec4(.45f, .74f, .38f, 1), ImVec4(.38f, .56f, .93f, 1), ImVec4(.72f, .58f, .9f, 1)};
    static const char* labels[4] = {"X", "Y", "Z", "W"};
    const float gap = 4.0f, tag = 16.0f;
    const float width = std::max(1.0f, (size.x - gap * (count - 1)) / count);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    StyleScope local;
    local.Var(ImGuiStyleVar_FramePadding, FitFramePadding(size.y));
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        if (i) ImGui::SameLine(0, gap);
        ImGui::SetNextItemWidth(width);
        u.valueChanged |= ImGui::DragFloat("##component", &u.numericValue[i], u.numericStep, u.sliderMin, u.sliderMax, "%.3f");
        ItemState(u);
        // Axis tag on the left edge, Unity/Blender style.
        const ImVec2 a = ImGui::GetItemRectMin(), b(a.x + tag, ImGui::GetItemRectMax().y);
        const float r = std::max(0.0f, u.rounding);
        dl->AddRectFilled(a, b, U32(WithAlpha(axes[i], .85f)), r, ImDrawFlags_RoundCornersLeft);
        CenteredText(dl, a, b, IM_COL32(255, 255, 255, 235), labels[i]);
        ImGui::PopID();
    }
}
inline void ObjectField(UIElementComponent& u, std::vector<SceneObject>& scene, ImVec2 size) {
    SceneObject* target = Find(scene, u.linkedObjectId);
    std::string preview = target ? target->name : "None (Object)";
    StyleScope local;
    local.Var(ImGuiStyleVar_FramePadding, FitFramePadding(size.y));
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!target) local.Color(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::BeginCombo("##object", preview.c_str(), ImGuiComboFlags_HeightLarge);
    if (!target) local.Color(ImGuiCol_Text, text); // the popup rows use the normal color
    if (open) {
        // Searchable picker, the same shape as the editor's reference slot popup.
        std::string& filter = u.filterText;
        if (ImGui::IsWindowAppearing()) { filter.clear(); ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(-FLT_MIN);
        EditText("##filter", filter, "Search objects...");
        if (ImGui::Selectable("None", !target)) { u.linkedObjectId = -1; u.valueChanged = true; }
        for (const auto& candidate : scene) {
            if (!Match(candidate.name, filter)) continue;
            ImGui::PushID(candidate.id);
            if (ImGui::Selectable(candidate.name.c_str(), candidate.id == u.linkedObjectId)) { u.linkedObjectId = candidate.id; u.valueChanged = true; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ItemState(u);
}
inline void PropertyGrid(UIElementComponent& u, std::vector<SceneObject>& scene, ImVec2 size, const Palette& pal) {
    SceneObject* target = Find(scene, u.linkedObjectId);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + size.x, min.y + size.y);
    if (!target) {
        dl->AddRectFilled(min, max, U32(pal.background), std::max(0.0f, u.rounding));
        dl->AddRect(min, max, U32(pal.border), std::max(0.0f, u.rounding));
        CenteredText(dl, min, max, U32(pal.textDim), "No object linked");
        return;
    }
    StyleScope local;
    local.Var(ImGuiStyleVar_CellPadding, ImVec2(6, 3));
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##properties", 2, flags, size)) return;
    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, .38f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, .62f);
    auto row = [&](const char* label) {
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal.textDim, "%s", label); ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::PushID(label);
    };
    row("Name"); u.valueChanged |= EditText("##value", target->name); ImGui::PopID();
    row("Enabled"); u.valueChanged |= ImGui::Checkbox("##value", &target->enabled); ImGui::PopID();
    ImGui::BeginDisabled(target->IsInvariable);
    bool transformChanged = false;
    // UI transforms already use their own canonical component fields.
    if (target->hasUI) {
        row("Position"); u.valueChanged |= ImGui::DragFloat2("##value", &target->ui.position.x, 1); ImGui::PopID();
        row("Size"); u.valueChanged |= ImGui::DragFloat2("##value", &target->ui.size.x, 1, .01f, 65536); ImGui::PopID();
        row("Tint"); u.valueChanged |= ImGui::ColorEdit4("##value", &target->ui.color.x, ImGuiColorEditFlags_AlphaPreviewHalf); ImGui::PopID();
        row("Label"); u.valueChanged |= EditText("##value", target->ui.label); ImGui::PopID();
    } else {
        row("Position"); transformChanged |= ImGui::DragFloat3("##value", &target->position.x, .1f); ImGui::PopID();
        row("Rotation"); transformChanged |= ImGui::DragFloat3("##value", &target->rotation.x, .1f); ImGui::PopID();
        row("Scale"); transformChanged |= ImGui::DragFloat3("##value", &target->scale.x, .1f); ImGui::PopID();
        if (transformChanged) { target->localInitialized = false; u.valueChanged = true; }
    }
    ImGui::EndDisabled();
    ImGui::EndTable();
}
inline void FolderIcon(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    dl->AddRectFilled(ImVec2(p.x, p.y + s * .1f), ImVec2(p.x + s * .45f, p.y + s * .3f), col, 1.5f);
    dl->AddRectFilled(ImVec2(p.x, p.y + s * .22f), ImVec2(p.x + s, p.y + s * .85f), col, 2.0f);
}
inline void FileIcon(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    dl->AddRect(ImVec2(p.x + s * .15f, p.y), ImVec2(p.x + s * .8f, p.y + s * .9f), col, 2.0f, 0, 1.2f);
    dl->AddLine(ImVec2(p.x + s * .3f, p.y + s * .35f), ImVec2(p.x + s * .65f, p.y + s * .35f), col);
    dl->AddLine(ImVec2(p.x + s * .3f, p.y + s * .55f), ImVec2(p.x + s * .65f, p.y + s * .55f), col);
}
inline void FileBrowser(UIElementComponent& u, ImVec2 size, const Palette& pal, bool allowInput) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float row = ImGui::GetFrameHeight();
    const float top = ImGui::GetCursorScreenPos().y;
    bool refresh = false, up = false;
    {
        StyleScope local;
        local.Var(ImGuiStyleVar_ItemSpacing, ImVec2(4, 6));
        up = ImGui::Button("Up", ImVec2(0, row));
        ImGui::SameLine();
        const float refreshWidth = ImGui::CalcTextSize("Refresh").x + style.FramePadding.x * 2;
        ImGui::SetNextItemWidth(std::max(40.0f, size.x - ImGui::GetItemRectSize().x - refreshWidth - 8));
        bool submitted = false;
        EditText("##path", u.inputText, "Folder path", false, false, ImVec2(0, 0), &submitted);
        ImGui::SameLine();
        refresh = ImGui::Button("Refresh", ImVec2(0, row)) || submitted;
        ImGui::Checkbox("Folders only", &u.checked);
        if (!u.placeholder.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.95f, .45f, .45f, 1), "%s", u.placeholder.c_str()); }
    }
    if (up) { u.inputText = fs::path(u.inputText).parent_path().string(); refresh = true; }
    auto readDirectory = [&]() {
        std::error_code ec;
        fs::path path = u.inputText.empty() ? fs::current_path(ec) : fs::path(u.inputText);
        std::vector<std::string> entries;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code typeError;
            const bool directory = it->is_directory(typeError);
            if (!typeError && (directory || !u.checked)) entries.push_back(it->path().filename().string() + (directory ? "/" : ""));
        }
        // Folders first, then files, each alphabetical.
        std::sort(entries.begin(), entries.end(), [](const std::string& a, const std::string& b) {
            const bool da = !a.empty() && a.back() == '/', db = !b.empty() && b.back() == '/';
            return da != db ? da : a < b;
        });
        if (!ec) { u.items = std::move(entries); u.placeholder.clear(); u.inputText = path.string(); }
        else u.placeholder = ec.message();
        u.selectedIndex = -1;
    };
    // A browser shows its folder as soon as the game runs instead of waiting for a click.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID loadedKey = ImGui::GetID("##loaded");
    if (allowInput && !storage->GetBool(loadedKey) && u.items.empty()) refresh = true;
    if (refresh) { readDirectory(); storage->SetBool(loadedKey, true); }
    std::string navigateTo;
    const float listHeight = std::max(1.0f, size.y - (ImGui::GetCursorScreenPos().y - top));
    StyleScope local;
    local.Var(ImGuiStyleVar_WindowPadding, ImVec2(4, 4)); local.Var(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
    if (ImGui::BeginChild("##files", ImVec2(size.x, listHeight), ImGuiChildFlags_Borders)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (u.items.empty()) {
            const ImVec2 wmin = ImGui::GetWindowPos(), wsize = ImGui::GetWindowSize();
            CenteredText(dl, wmin, ImVec2(wmin.x + wsize.x, wmin.y + wsize.y), U32(pal.textDim), allowInput ? "Empty folder" : "Lists the folder at runtime");
        }
        const float h = ImGui::GetFontSize() + 6, icon = ImGui::GetFontSize() * .9f;
        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(u.items.size()), h + 1);
        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            ImGui::PushID(i);
            const std::string& entry = u.items[i];
            const bool directory = !entry.empty() && entry.back() == '/';
            if (ImGui::Selectable("##entry", u.selectedIndex == i, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, h))) {
                Select(u, i);
                if (ImGui::IsMouseDoubleClicked(0) && directory) navigateTo = (fs::path(u.inputText) / entry).string();
            }
            const ImVec2 rmin = ImGui::GetItemRectMin();
            const ImVec2 iconPos(rmin.x + 3, rmin.y + (h - icon) * .5f);
            if (directory) FolderIcon(dl, iconPos, icon, U32(Mix(pal.accent, pal.text, .2f)));
            else FileIcon(dl, iconPos, icon, U32(pal.textDim));
            dl->AddText(ImVec2(rmin.x + icon + 9, rmin.y + (h - ImGui::GetFontSize()) * .5f), U32(pal.text),
                        entry.c_str(), directory ? entry.c_str() + entry.size() - 1 : nullptr);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (!navigateTo.empty()) { u.inputText = navigateTo; readDirectory(); }
}
inline void TabView(UIElementComponent& u, ImVec2 min, ImVec2 max, const Palette& pal) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(max.x - min.x, max.y - min.y);
    const float r = std::max(0.0f, u.rounding);
    dl->AddRectFilled(min, max, U32(pal.background), r);
    if (u.borderWidth > 0) dl->AddRect(min, max, U32(pal.border), r, 0, u.borderWidth);
    StyleScope local;
    local.Color(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    local.Var(ImGuiStyleVar_WindowPadding, ImVec2(6, 5)); local.Var(ImGuiStyleVar_FramePadding, ImVec2(12, 5));
    local.Var(ImGuiStyleVar_TabRounding, std::min(r, 6.0f));
    const float header = std::min(size.y, ImGui::GetFontSize() + 10 + 10);
    if (ImGui::BeginChild("##tabHeader", ImVec2(size.x, header), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // Follow selection changes made by scripts or the inspector, not only clicks.
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID shownKey = ImGui::GetID("##shown");
        const bool external = storage->GetInt(shownKey, -1) != u.selectedIndex;
        if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
            for (int i = 0; i < static_cast<int>(u.items.size()); ++i) {
                ImGui::PushID(i);
                const ImGuiTabItemFlags flags = external && i == u.selectedIndex ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(u.items[i].c_str(), nullptr, flags)) {
                    if (u.selectedIndex != i && !external) { u.selectedIndex = i; u.valueChanged = true; }
                    ImGui::EndTabItem();
                }
                ItemState(u);
                ImGui::PopID();
            }
            ImGui::EndTabBar();
        }
        storage->SetInt(shownKey, u.selectedIndex);
    }
    ImGui::EndChild();
}
inline void Modal(UIElementComponent& u, ImVec2 min, ImVec2 max, const Palette& pal, bool allowInput) {
    const ImVec2 size(max.x - min.x, max.y - min.y);
    const float r = std::max(0.0f, u.rounding);
    if (!allowInput) {
        // Edit-time preview of the dialog where it will appear.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float title = ImGui::GetFontSize() + 12;
        ImGui::ShadeRect(dl, min, max, U32(WithAlpha(Lift(pal.background, .04f), 1)), ImGuiShadeClass_Popup, ImGuiShadeState_Normal, r);
        dl->AddRect(min, max, U32(pal.border), r);
        dl->AddLine(ImVec2(min.x, min.y + title), ImVec2(max.x, min.y + title), U32(pal.border));
        dl->AddText(ImVec2(min.x + 10, min.y + 6), U32(pal.text), u.label.c_str());
        dl->AddText(nullptr, 0, ImVec2(min.x + 10, min.y + title + 8), U32(pal.textDim), u.inputText.c_str(), nullptr, size.x - 20);
        const ImVec2 bmax(max.x - 10, max.y - 10), bmin(bmax.x - 64, bmax.y - ImGui::GetFrameHeight());
        ImGui::ShadeRect(dl, bmin, bmax, U32(pal.accent), ImGuiShadeClass_Button, ImGuiShadeState_Normal, std::min(r, 6.0f));
        CenteredText(dl, bmin, bmax, U32(pal.onAccent), "OK");
        if (!u.open) CenteredText(dl, ImVec2(min.x, min.y), ImVec2(max.x, min.y + title), U32(pal.textDim), "(closed)");
        return;
    }
    // Popup windows are looked up by name, so make it unique per control even when titles match.
    const std::string title = u.label + "###UIModal" + std::to_string(ImGui::GetID("##modal"));
    if (u.open && !ImGui::IsPopupOpen(title.c_str())) ImGui::OpenPopup(title.c_str());
    ImGui::SetNextWindowPos(ImVec2((min.x + max.x) * .5f, (min.y + max.y) * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    StyleScope local;
    local.Var(ImGuiStyleVar_WindowRounding, r); local.Var(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    local.Color(ImGuiCol_TitleBg, Lift(pal.background, .06f)); local.Color(ImGuiCol_TitleBgActive, Lift(pal.background, .1f));
    if (ImGui::BeginPopupModal(title.c_str(), &u.open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(u.inputText.c_str());
        ImGui::PopTextWrapPos();
        const float bw = 72, bh = ImGui::GetFrameHeight();
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - bw - 12, std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - bh - 10)));
        StyleScope button;
        button.Color(ImGuiCol_Button, pal.accent); button.Color(ImGuiCol_ButtonHovered, Lift(pal.accent, .12f));
        button.Color(ImGuiCol_ButtonActive, Mix(pal.accent, ImVec4(0, 0, 0, 1), .15f)); button.Color(ImGuiCol_Text, pal.onAccent);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("OK", ImVec2(bw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            u.open = false; u.buttonPressed = true; ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
#pragma endregion

inline bool Draw(SceneObject& obj, std::vector<SceneObject>& scene,
                 ImVec2 min, ImVec2 max, bool allowInput = true, ImTextureID panelTexture = 0,
                 const std::function<void(SceneObject&, ImVec2, ImVec2)>& drawDockedCanvas = {}) {
    auto& u = obj.ui;
    if (!IsExtendedUIType(u.type)) return false;
    u.buttonPressed = u.inputSubmitted = u.valueChanged = false;
    u.uiHovered = u.uiActive = u.uiFocused = false;
    ImVec2 size(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y));
    ImGui::SetCursorScreenPos(min);
    ImGui::PushClipRect(min, max, true);
    ImGui::PushItemWidth(size.x);
    const Palette pal = ResolvePalette(u);
    {
    StyleScope style;
    ApplyPalette(style, pal, u);
    // Outside play mode controls are a preview: inert, but not greyed out like a disabled control.
    if (!allowInput && u.interactable) style.Var(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(!u.interactable || !allowInput);
    if (u.requestFocus && allowInput && u.interactable) { ImGui::SetKeyboardFocusHere(); u.requestFocus = false; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = std::max(0.0f, u.rounding);
    const bool customFont = u.fontSize > 0 || u.textScale != 1;
    if (customFont) ImGui::PushFont(ImGui::GetFont(), u.fontSize > 0 ? u.fontSize : ImGui::GetFontSize() * std::max(.1f, u.textScale));
    // Single-line frame controls are vertically centered in, and fill, their rect.
    auto fitFrame = [&](StyleScope& s) { s.Var(ImGuiStyleVar_FramePadding, FitFramePadding(size.y)); };
    auto centerRow = [&](float height) { ImGui::SetCursorScreenPos(ImVec2(min.x, std::floor(min.y + std::max(0.0f, size.y - height) * .5f))); };
    ImGui::BeginGroup();
    switch (u.type) {
    case UIElementType::DockContainer: {
        if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable)) break;
        const ImGuiID dockspace = ImGui::GetID("##dockspace");
        if (ImGui::BeginChild("##dockHost", size, ImGuiChildFlags_Borders)) {
            for (auto& panel : scene) {
                if (panel.parentId != obj.id || !panel.hasUI || panel.ui.type != UIElementType::Canvas) continue;
                ImGui::PushID(panel.id);
                if (!panel.ui.open && ImGui::SmallButton(panel.name.c_str())) panel.ui.open = true;
                ImGui::PopID();
            }
            ImGui::DockSpace(dockspace, ImVec2(0, 0));
        }
        ImGui::EndChild();
        for (auto& panel : scene) {
            if (panel.parentId != obj.id || !panel.hasUI || panel.ui.type != UIElementType::Canvas || !panel.enabled || !panel.ui.open) continue;
            const std::string title = panel.name + "###DockUI_" + std::to_string(obj.id) + "_" + std::to_string(panel.id);
            ImGui::SetNextWindowDockID(dockspace, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(320, 240), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(title.c_str(), &panel.ui.open, ImGuiWindowFlags_NoSavedSettings)) {
                const ImVec2 contentMin = ImGui::GetCursorScreenPos();
                const ImVec2 available = ImGui::GetContentRegionAvail();
                if (drawDockedCanvas && available.x > 1 && available.y > 1)
                    drawDockedCanvas(panel, contentMin, ImVec2(contentMin.x + available.x, contentMin.y + available.y));
            }
            ImGui::End();
        }
        break;
    }
    case UIElementType::Tooltip:
        ImGui::InvisibleButton("##tooltip", size); ItemState(u);
        if (!allowInput) { // invisible at runtime; outline the hover area while editing
            DashedRect(dl, min, max, U32(WithAlpha(pal.accent, .6f)));
            CenteredText(dl, min, max, U32(pal.textDim), "?");
        }
        break;
    case UIElementType::Panel:
    case UIElementType::ScrollView: {
        dl->AddRectFilled(min, max, U32(ToIm(u.backgroundColor.a > 0 ? u.backgroundColor : u.color)), rounding);
        if (panelTexture != 0) dl->AddImageRounded(panelTexture, min, max, ImVec2(0, 0), ImVec2(1, 1), U32(ToIm(u.color)), rounding);
        if (u.borderWidth > 0) dl->AddRect(min, max, U32(pal.border), rounding, 0, u.borderWidth);
        if (u.type == UIElementType::ScrollView && allowInput && u.interactable && ImGui::IsMouseHoveringRect(min, max)) {
            const auto& io = ImGui::GetIO();
            u.scrollOffset.y -= io.MouseWheel * std::max(1.0f, u.rowHeight) * 2;
            u.scrollOffset.x -= io.MouseWheelH * std::max(1.0f, u.rowHeight) * 2;
            u.scrollOffset = glm::clamp(u.scrollOffset, glm::vec2(0), glm::max(glm::vec2(0), u.contentSize - u.size));
        }
        break;
    }
    case UIElementType::InputField: {
        StyleScope local; if (!u.multiline) fitFrame(local);
        u.valueChanged = EditText("##input", u.inputText, u.placeholder.c_str(), u.multiline, u.password, size, &u.inputSubmitted);
        if (u.inputSubmitted) ++u.inputSubmitRevision;
        ItemState(u);
        break;
    }
    case UIElementType::SearchField: {
        const float iconArea = std::min(size.y, 26.0f);
        {
            StyleScope local;
            local.Var(ImGuiStyleVar_FramePadding, ImVec2(iconArea, FitFramePadding(size.y).y));
            if (!u.inputText.empty()) ImGui::SetNextItemAllowOverlap();
            u.valueChanged = EditText("##input", u.inputText, u.placeholder.c_str(), false, false, size, &u.inputSubmitted);
        }
        if (u.inputSubmitted) ++u.inputSubmitRevision;
        ItemState(u);
        // Magnifier glyph in the left padding.
        const ImVec2 c(min.x + iconArea * .5f, min.y + size.y * .5f - 1);
        const float lens = std::max(3.0f, ImGui::GetFontSize() * .26f);
        dl->AddCircle(c, lens, U32(pal.textDim), 0, 1.5f);
        dl->AddLine(ImVec2(c.x + lens * .7f, c.y + lens * .7f), ImVec2(c.x + lens * 1.6f, c.y + lens * 1.6f), U32(pal.textDim), 1.8f);
        if (!u.inputText.empty()) {
            const float clear = std::min(size.y, 26.0f);
            ImGui::SetCursorScreenPos(ImVec2(max.x - clear, min.y + (size.y - clear) * .5f));
            if (ImGui::InvisibleButton("##clear", ImVec2(clear, clear))) { u.inputText.clear(); u.valueChanged = true; }
            const ImVec2 cc(max.x - clear * .5f, min.y + size.y * .5f);
            const ImU32 xcol = U32(ImGui::IsItemHovered() ? pal.text : pal.textDim);
            dl->AddLine(ImVec2(cc.x - 4, cc.y - 4), ImVec2(cc.x + 4, cc.y + 4), xcol, 1.5f);
            dl->AddLine(ImVec2(cc.x - 4, cc.y + 4), ImVec2(cc.x + 4, cc.y - 4), xcol, 1.5f);
        }
        if (u.linkedObjectId >= 0)
            if (auto* target = Find(scene, u.linkedObjectId)) target->ui.filterText = u.inputText;
        break;
    }
    case UIElementType::Toggle:
        centerRow(ImGui::GetFrameHeight());
        u.valueChanged = ImGui::Checkbox(u.label.c_str(), &u.checked); ItemState(u); break;
    case UIElementType::RadioGroup: {
        StyleScope local; local.Var(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
        for (int i = 0; i < static_cast<int>(u.items.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::RadioButton(u.items[i].c_str(), u.selectedIndex == i)) Select(u, i);
            ItemState(u); ImGui::PopID();
        }
        break;
    }
    case UIElementType::ProgressBar: ProgressBar(u, min, max, pal); break;
    case UIElementType::RangeSlider: RangeSlider(u, min, max, pal); ItemState(u); break;
    case UIElementType::Separator:
        dl->AddLine(u.vertical ? ImVec2(min.x + size.x * .5f, min.y) : ImVec2(min.x, min.y + size.y * .5f),
                    u.vertical ? ImVec2(min.x + size.x * .5f, max.y) : ImVec2(max.x, min.y + size.y * .5f),
                    U32(ToIm(u.color)), std::max(1.0f, u.borderWidth)); break;
    case UIElementType::Scrollbar: Scrollbar(u, scene, min, max, pal); ItemState(u); break;
    case UIElementType::Dropdown:
    case UIElementType::EnumDropdown: {
        const bool hasSelection = u.selectedIndex >= 0 && u.selectedIndex < static_cast<int>(u.items.size());
        const char* preview = hasSelection ? u.items[u.selectedIndex].c_str() : u.placeholder.c_str();
        if (u.type == UIElementType::Dropdown && Find(scene, u.linkedObjectId)) {
            // Template dropdown: the popup is a scene-owned child list shown while open.
            u.buttonPressed = DropdownFace("##dropdown", preview, !hasSelection, min, size, pal, rounding, u.open);
            if (u.buttonPressed) {
                u.open = !u.open;
                if (auto* list = Find(scene, u.linkedObjectId)) list->ui.requestFocus = u.open;
            }
            ItemState(u);
            if (u.open && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(min,
                    ImVec2(max.x, max.y + std::max(0.f, u.contentSize.y)))) u.open = false;
            break;
        }
        StyleScope local; fitFrame(local);
        local.Var(ImGuiStyleVar_WindowPadding, ImVec2(4, 4)); local.Var(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));
        if (!hasSelection) local.Color(ImGuiCol_Text, pal.textDim);
        const bool open = ImGui::BeginCombo("##dropdown", preview);
        if (!hasSelection) local.Color(ImGuiCol_Text, pal.text);
        if (open) {
            StyleScope rows; rows.Var(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, .5f));
            for (int i = 0; i < static_cast<int>(u.items.size()); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(u.items[i].c_str(), u.selectedIndex == i, 0, ImVec2(0, std::max(ImGui::GetFontSize() + 6, u.rowHeight)))) Select(u, i);
                if (u.selectedIndex == i) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ItemState(u); break;
    }
    case UIElementType::ListView: case UIElementType::GridView: case UIElementType::TreeView:
    case UIElementType::ReorderableList: case UIElementType::LogView: {
        SceneObject* source = u.type == UIElementType::ListView ? Find(scene, u.linkedObjectId) : nullptr;
        if (source && source->ui.type == UIElementType::Dropdown) {
            source->ui.buttonPressed = false;
            Collection(source->ui, size, pal);
            if (source->ui.buttonPressed) { source->ui.open = false; ++source->ui.valueChangeRevision; }
        } else Collection(u, size, pal);
        break;
    }
    case UIElementType::MultiColumnListView: {
        const int columns = std::clamp(u.gridColumns, 1, 1024);
        StyleScope local; local.Var(ImGuiStyleVar_CellPadding, ImVec2(6, 2)); local.Var(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, .5f));
        dl->AddRectFilled(min, max, U32(pal.background), rounding);
        ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (u.sortable) flags |= ImGuiTableFlags_Sortable;
        if (ImGui::BeginTable("##table", columns, flags, size)) {
            // Column titles come from the label as "Name|Type|Value".
            const char* header = u.label.find('|') != std::string::npos ? u.label.c_str() : nullptr;
            for (int c = 0; c < columns; ++c) {
                const char* end = header ? std::strchr(header, '|') : nullptr;
                const std::string name = header ? std::string(header, end ? end : header + std::strlen(header)) : "Column " + std::to_string(c + 1);
                ImGui::TableSetupColumn(name.c_str());
                header = header && end ? end + 1 : nullptr;
            }
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            if (auto* sort = ImGui::TableGetSortSpecs()) {
                if (sort->SpecsDirty && sort->SpecsCount > 0) {
                    int column = sort->Specs[0].ColumnIndex;
                    bool ascending = sort->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                    auto cell = [column](const std::string& row) {
                        size_t start = 0;
                        for (int c = 0; c < column; ++c) {
                            size_t next = row.find('|', start); if (next == std::string::npos) return std::string_view{}; start = next + 1;
                        }
                        size_t end = row.find('|', start);
                        return std::string_view(row.data() + start, (end == std::string::npos ? row.size() : end) - start);
                    };
                    std::string selected = u.selectedIndex >= 0 && u.selectedIndex < static_cast<int>(u.items.size()) ? u.items[u.selectedIndex] : std::string{};
                    std::stable_sort(u.items.begin(), u.items.end(), [&](const std::string& a, const std::string& b) { return ascending ? cell(a) < cell(b) : cell(b) < cell(a); });
                    auto found = std::find(u.items.begin(), u.items.end(), selected);
                    u.selectedIndex = found == u.items.end() ? -1 : static_cast<int>(found - u.items.begin());
                    sort->SpecsDirty = false; u.valueChanged = true;
                }
            }
            auto& visible = u.visibleItemIndices; visible.clear();
            const bool filtered = !u.filterText.empty();
            if (filtered) {
                visible.reserve(u.items.size());
                for (int i = 0; i < static_cast<int>(u.items.size()); ++i) if (Match(u.items[i], u.filterText)) visible.push_back(i);
            }
            const int count = static_cast<int>(filtered ? visible.size() : u.items.size());
            const float rowHeight = std::max(ImGui::GetFontSize() + 4, u.rowHeight);
            ImGuiListClipper clipper; clipper.Begin(count, rowHeight);
            if (!u.virtualize) clipper.IncludeItemsByIndex(0, count);
            while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                int i = filtered ? visible[row] : row;
                ImGui::TableNextRow(0, rowHeight);
                const char* begin = u.items[i].c_str();
                for (int c = 0; c < columns; ++c) {
                    ImGui::TableSetColumnIndex(c);
                    const char* end = std::strchr(begin, '|');
                    ImGui::PushID(i * columns + c);
                    if (c == 0) {
                        const std::string text(begin, end ? end : begin + std::strlen(begin));
                        if (ImGui::Selectable(text.c_str(), u.selectedIndex == i, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, rowHeight - 4))) Select(u, i);
                        ItemState(u);
                    } else {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (rowHeight - 4 - ImGui::GetFontSize()) * .5f));
                        ImGui::TextUnformatted(begin, end);
                    }
                    ImGui::PopID(); begin = end ? end + 1 : begin + std::strlen(begin);
                }
            }
            ImGui::EndTable();
        }
        break;
    }
    case UIElementType::TabView: TabView(u, min, max, pal); break;
    case UIElementType::Foldout: Foldout(u, min, max, pal); ItemState(u); break;
    case UIElementType::Toolbar: Toolbar(u, min, max, pal); break;
    case UIElementType::Breadcrumb: Breadcrumb(u, min, max, pal); break;
    case UIElementType::MenuBar: MenuBar(u, min, max, pal); break;
    case UIElementType::PopupMenu: {
        if (DropdownFace("##trigger", u.label.c_str(), false, min, size, pal, rounding, ImGui::IsPopupOpen("##popup"), ImGuiShadeClass_Button))
            ImGui::OpenPopup("##popup");
        ItemState(u);
        if (ImGui::BeginPopupContextItem("##context")) { MenuItems(u); ImGui::EndPopup(); }
        ImGui::SetNextWindowPos(ImVec2(min.x, max.y + 2));
        ImGui::SetNextWindowSizeConstraints(ImVec2(size.x, 0), ImVec2(FLT_MAX, FLT_MAX));
        if (ImGui::BeginPopup("##popup")) { MenuItems(u); ImGui::EndPopup(); }
        break;
    }
    case UIElementType::Modal: Modal(u, min, max, pal, allowInput); break;
    case UIElementType::ColorPicker: {
        // Fit the square, hue/alpha bars and the three RGB/HSV/HEX rows inside the rect.
        const ImGuiStyle& st = ImGui::GetStyle();
        const float frame = ImGui::GetFrameHeight();
        const float rowsHeight = 3 * (frame + st.ItemSpacing.y) + st.ItemInnerSpacing.y;
        const float bars = 2 * (frame + st.ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(std::max(frame * 4, std::min(size.x, size.y - rowsHeight + bars)));
        u.valueChanged = ImGui::ColorPicker4("##color", &u.numericValue.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview |
            ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_AlphaPreviewHalf);
        ItemState(u); break;
    }
    case UIElementType::NumericField: {
        StyleScope local; fitFrame(local);
        u.valueChanged = ImGui::DragFloat("##number", &u.numericValue.x, u.numericStep, u.sliderMin, u.sliderMax, "%.3f"); ItemState(u); break;
    }
    case UIElementType::Vector2Field: VectorField(u, 2, size); break;
    case UIElementType::Vector3Field: VectorField(u, 3, size); break;
    case UIElementType::Vector4Field: VectorField(u, 4, size); break;
    case UIElementType::ObjectField: ObjectField(u, scene, size); break;
    case UIElementType::PropertyGrid: PropertyGrid(u, scene, size, pal); break;
    case UIElementType::FileBrowser: FileBrowser(u, size, pal, allowInput); break;
    case UIElementType::SplitView: SplitHandle(u, min, max, pal); ItemState(u); break;
    default: break;
    }
    ImGui::EndGroup();
    if (u.type != UIElementType::Panel && u.type != UIElementType::ScrollView && u.type != UIElementType::Separator)
        ItemState(u);
    if (u.valueChanged) ++u.valueChangeRevision;
    if (u.type == UIElementType::Toggle) u.buttonPressed = u.valueChanged;
    if (customFont) ImGui::PopFont();
    // Custom-drawn panels/separators still register their extent with ImGui.
    ImGui::SetCursorScreenPos(min); ImGui::Dummy(size);
    ImGui::EndDisabled();
    } // pops the palette
    if (allowInput && !u.tooltip.empty() && ImGui::IsMouseHoveringRect(min, max) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        ImGui::SetTooltip("%s", u.tooltip.c_str());
    ImGui::PopItemWidth();
    ImGui::PopClipRect();
    return true;
}
// Tab and foldout contents are ordinary scene children; scripts can add/remove them.
inline bool IsDockedCanvas(const SceneObject& obj, const std::vector<SceneObject>& scene) {
    if (!obj.hasUI || obj.ui.type != UIElementType::Canvas) return false;
    auto parent = std::find_if(scene.begin(), scene.end(), [&](const SceneObject& node) { return node.id == obj.parentId; });
    return parent != scene.end() && parent->hasUI && parent->ui.type == UIElementType::DockContainer;
}
inline bool ContentVisible(const SceneObject& obj, const std::vector<SceneObject>& scene) {
    const SceneObject* node = &obj;
    for (size_t depth = 0; depth < scene.size() && node->parentId >= 0; ++depth) {
        auto it = std::find_if(scene.begin(), scene.end(), [node](const SceneObject& p) { return p.id == node->parentId; });
        if (it == scene.end()) break;
        if (it->hasUI && it->ui.type == UIElementType::Foldout && !it->ui.open) return false;
        if (it->hasUI && it->ui.type == UIElementType::Dropdown && it->ui.linkedObjectId >= 0 && !it->ui.open) return false;
        if (it->hasUI && it->ui.type == UIElementType::TabView) {
            int index = 0;
            for (const auto& sibling : scene) {
                if (sibling.parentId != it->id || !sibling.hasUI) continue;
                if (sibling.id == node->id) break;
                ++index;
            }
            if (index != it->ui.selectedIndex) return false;
        }
        node = &*it;
    }
    return true;
}
} // namespace UIControls
