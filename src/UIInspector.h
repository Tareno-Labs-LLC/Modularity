#pragma once
#include "UIControls.h"

namespace UIControls {
// The editor inspector's own widgets, passed in so this header stays free of Engine.
// Unset hooks fall back to plain ImGui (tools and tests).
struct InspectorHooks {
    // Draws a collapsible subsection header and runs `body` while it is open.
    std::function<void(const char* label, bool defaultOpen, const std::function<void()>& body)> section;
    // A SceneOBJ reference slot (drag-drop, searchable picker). Returns true when changed.
    std::function<bool(const char* id, const char* label, int& objectId, const char* typeName)> objectSlot;
};

inline bool UsesLabel(UIElementType t) {
    return t == UIElementType::Toggle || t == UIElementType::ProgressBar || t == UIElementType::Foldout ||
           t == UIElementType::PopupMenu || t == UIElementType::Modal || t == UIElementType::MultiColumnListView;
}
inline bool UsesItems(UIElementType t) {
    switch (t) {
    case UIElementType::RadioGroup: case UIElementType::Dropdown: case UIElementType::EnumDropdown:
    case UIElementType::ListView: case UIElementType::GridView: case UIElementType::TreeView:
    case UIElementType::MultiColumnListView: case UIElementType::ReorderableList: case UIElementType::LogView:
    case UIElementType::TabView: case UIElementType::Toolbar: case UIElementType::Breadcrumb:
    case UIElementType::MenuBar: case UIElementType::PopupMenu: return true;
    default: return false;
    }
}
inline bool HierarchicalItems(UIElementType t) {
    return t == UIElementType::TreeView || t == UIElementType::MenuBar || t == UIElementType::PopupMenu;
}
inline bool IsCollection(UIElementType t) {
    return t == UIElementType::ListView || t == UIElementType::GridView || t == UIElementType::TreeView ||
           t == UIElementType::MultiColumnListView || t == UIElementType::ReorderableList || t == UIElementType::LogView;
}
// What a control's linked object means, or nullptr when it has none.
inline const char* LinkRole(UIElementType t, const char** typeName) {
    switch (t) {
    case UIElementType::Scrollbar: *typeName = "Scroll View"; return "Scrolls";
    case UIElementType::SearchField: *typeName = "List"; return "Filters";
    case UIElementType::Dropdown: *typeName = "List View"; return "Popup List";
    case UIElementType::ListView: *typeName = "Dropdown"; return "Items From";
    case UIElementType::ObjectField: *typeName = "SceneOBJ"; return "Value";
    case UIElementType::PropertyGrid: *typeName = "SceneOBJ"; return "Target";
    default: return nullptr;
    }
}
// Moves an item and remaps everything that refers to items by index.
inline void MoveItem(UIElementComponent& u, int from, int to) {
    const int n = static_cast<int>(u.items.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, from);
    std::vector<int> newIndex(n);
    for (int i = 0; i < n; ++i) newIndex[order[i]] = i;
    std::vector<std::string> items(n);
    for (int i = 0; i < n; ++i) items[i] = std::move(u.items[order[i]]);
    u.items = std::move(items);
    if (!u.itemParents.empty()) {
        u.itemParents.resize(n, -1);
        std::vector<int> parents(n);
        for (int i = 0; i < n; ++i) { int p = u.itemParents[order[i]]; parents[i] = p >= 0 && p < n ? newIndex[p] : -1; }
        u.itemParents = std::move(parents);
    }
    for (int& e : u.expandedItems) if (e >= 0 && e < n) e = newIndex[e];
    if (u.selectedIndex >= 0 && u.selectedIndex < n) u.selectedIndex = newIndex[u.selectedIndex];
}
inline void RemoveItem(UIElementComponent& u, int remove) {
    u.items.erase(u.items.begin() + remove);
    if (remove < static_cast<int>(u.itemParents.size())) u.itemParents.erase(u.itemParents.begin() + remove);
    for (auto& p : u.itemParents) { if (p == remove) p = -1; else if (p > remove) --p; }
    u.expandedItems.clear();
    if (u.selectedIndex == remove) u.selectedIndex = -1;
    else if (u.selectedIndex > remove) --u.selectedIndex;
}
// One row per item: drag handle, label, parent index for trees/menus, remove button.
inline bool ItemsEditor(UIElementComponent& u) {
    bool changed = false;
    const bool hierarchical = HierarchicalItems(u.type);
    if (hierarchical && u.itemParents.size() < u.items.size()) u.itemParents.resize(u.items.size(), -1);
    const float row = ImGui::GetFrameHeight();
    const float parentWidth = hierarchical ? 64.0f : 0.0f;
    int remove = -1, moveFrom = -1, moveTo = -1;
    for (int i = 0; i < static_cast<int>(u.items.size()); ++i) {
        ImGui::PushID(i);
        // Drag handle: dropping on another row moves this one there.
        ImGui::Button("=", ImVec2(row * .8f, row));
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("MODU_UI_ITEM_ROW", &i, sizeof(int));
            ImGui::TextUnformatted(u.items[i].c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODU_UI_ITEM_ROW"))
                if (payload->DataSize == sizeof(int)) { moveFrom = *static_cast<const int*>(payload->Data); moveTo = i; }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to reorder");
        ImGui::SameLine(0, 4);
        if (hierarchical && u.itemParents[i] >= 0) { ImGui::Dummy(ImVec2(8, row)); ImGui::SameLine(0, 0); } // children indent
        ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - parentWidth - row - 8));
        changed |= EditText("##label", u.items[i]);
        if (hierarchical) {
            ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(parentWidth);
            int parent = u.itemParents[i];
            if (ImGui::DragInt("##parent", &parent, .1f, -1, static_cast<int>(u.items.size()) - 1, parent < 0 ? "Root" : "In %d")) {
                if (parent != i) { u.itemParents[i] = parent; changed = true; }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Parent item index (Root = top level)");
        }
        ImGui::SameLine(0, 4);
        if (ImGui::Button("x", ImVec2(row, row))) remove = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");
        ImGui::PopID();
    }
    if (moveFrom >= 0 && moveFrom != moveTo) { MoveItem(u, moveFrom, moveTo); changed = true; }
    if (remove >= 0) { RemoveItem(u, remove); changed = true; }
    if (ImGui::Button("+ Add Item", ImVec2(-FLT_MIN, 0))) {
        u.items.push_back("Item " + std::to_string(u.items.size() + 1));
        if (hierarchical) u.itemParents.resize(u.items.size(), -1);
        changed = true;
    }
    return changed;
}

inline bool Inspector(UIElementComponent& u, bool transformLocked = false, const InspectorHooks& hooks = {}) {
    bool changed = false;
    auto section = [&](const char* label, bool defaultOpen, const std::function<void()>& body) {
        if (hooks.section) { hooks.section(label, defaultOpen, body); return; }
        if (ImGui::TreeNodeEx(label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0)) { body(); ImGui::TreePop(); }
    };
    const UIElementType t = u.type;
    if (IsExtendedUIType(t)) {
        ImGui::TextDisabled("%s", TypeName(t));
        section("Content", true, [&] {
            if (UsesLabel(t)) {
                const char* label = t == UIElementType::Modal ? "Title" : t == UIElementType::MultiColumnListView ? "Columns" : "Label";
                changed |= EditText(label, u.label);
                if (t == UIElementType::MultiColumnListView && ImGui::IsItemHovered()) ImGui::SetTooltip("Column titles separated by |, e.g. Name|Type|Value");
            }
            switch (t) {
            case UIElementType::InputField: case UIElementType::SearchField:
                changed |= EditText("Text", u.inputText);
                changed |= EditText("Placeholder", u.placeholder);
                if (t == UIElementType::InputField) {
                    changed |= ImGui::Checkbox("Multiline", &u.multiline);
                    ImGui::SameLine(); changed |= ImGui::Checkbox("Password", &u.password);
                }
                break;
            case UIElementType::Toggle: changed |= ImGui::Checkbox("Checked", &u.checked); break;
            case UIElementType::Foldout: changed |= ImGui::Checkbox("Open", &u.open); break;
            case UIElementType::Modal:
                changed |= EditText("Message", u.inputText, "", true, false, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.5f));
                changed |= ImGui::Checkbox("Open", &u.open);
                ImGui::SameLine(); changed |= ImGui::Checkbox("Dim background", &u.dimBackground);
                break;
            case UIElementType::ProgressBar:
                changed |= ImGui::DragFloat("Value", &u.sliderValue, .01f, u.sliderMin, u.sliderMax);
                changed |= ImGui::DragFloatRange2("Range", &u.sliderMin, &u.sliderMax, .01f, 0, 0, "Min %.2f", "Max %.2f");
                changed |= ImGui::Checkbox("Indeterminate", &u.indeterminate);
                if (u.label.empty()) ImGui::TextDisabled("An empty label shows the percentage.");
                break;
            case UIElementType::RangeSlider:
                changed |= ImGui::DragFloatRange2("Selected", &u.sliderValue, &u.rangeUpper, .01f, u.sliderMin, u.sliderMax, "Low %.2f", "High %.2f");
                changed |= ImGui::DragFloatRange2("Bounds", &u.sliderMin, &u.sliderMax, .01f, 0, 0, "Min %.2f", "Max %.2f");
                break;
            case UIElementType::NumericField: case UIElementType::Vector2Field:
            case UIElementType::Vector3Field: case UIElementType::Vector4Field: {
                const int n = t == UIElementType::NumericField ? 1 : t == UIElementType::Vector2Field ? 2 : t == UIElementType::Vector3Field ? 3 : 4;
                changed |= ImGui::DragScalarN("Value", ImGuiDataType_Float, &u.numericValue.x, n, u.numericStep);
                changed |= ImGui::DragFloatRange2("Clamp", &u.sliderMin, &u.sliderMax, .1f, 0, 0, "Min %.2f", "Max %.2f");
                if (u.sliderMin == u.sliderMax) ImGui::TextDisabled("Equal bounds leave the value unclamped.");
                changed |= ImGui::DragFloat("Drag step", &u.numericStep, .01f, .00001f, 1000);
                break;
            }
            case UIElementType::ColorPicker:
                changed |= ImGui::ColorEdit4("Value", &u.numericValue.x, ImGuiColorEditFlags_AlphaPreviewHalf); break;
            case UIElementType::Scrollbar:
                changed |= ImGui::Checkbox("Vertical", &u.vertical);
                if (u.linkedObjectId < 0) {
                    changed |= ImGui::DragFloat("Scroll range", &u.sliderMax, 1, 0, 65536);
                    changed |= ImGui::DragFloat("Position", &u.sliderValue, 1, 0, std::max(0.0f, u.sliderMax));
                }
                break;
            case UIElementType::ScrollView:
                changed |= ImGui::DragFloat2("Content size", &u.contentSize.x, 1, 0, 65536);
                changed |= ImGui::DragFloat2("Scroll offset", &u.scrollOffset.x, 1, 0, 65536);
                break;
            case UIElementType::SplitView:
                changed |= ImGui::Checkbox("Vertical split", &u.vertical);
                changed |= ImGui::SliderFloat("Split", &u.sliderValue, .05f, .95f, "%.2f");
                break;
            case UIElementType::Separator: changed |= ImGui::Checkbox("Vertical", &u.vertical); break;
            case UIElementType::FileBrowser:
                changed |= EditText("Folder", u.inputText, "Working directory");
                changed |= ImGui::Checkbox("Folders only", &u.checked);
                break;
            case UIElementType::Tooltip:
                ImGui::TextDisabled("Shows the Tooltip text below while hovered.");
                break;
            default: break;
            }
            const char* linkType = nullptr;
            if (const char* role = LinkRole(t, &linkType)) {
                if (hooks.objectSlot) changed |= hooks.objectSlot("##uiLinked", role, u.linkedObjectId, linkType);
                else changed |= ImGui::InputInt(role, &u.linkedObjectId);
            }
            if (UsesItems(t)) {
                if (t != UIElementType::Breadcrumb && t != UIElementType::MenuBar && t != UIElementType::PopupMenu && t != UIElementType::LogView) {
                    int selected = u.selectedIndex;
                    const char* preview = selected >= 0 && selected < static_cast<int>(u.items.size()) ? u.items[selected].c_str() : "None";
                    if (ImGui::BeginCombo("Selected", preview)) {
                        if (ImGui::Selectable("None", selected < 0)) { u.selectedIndex = -1; changed = true; }
                        for (int i = 0; i < static_cast<int>(u.items.size()); ++i) {
                            ImGui::PushID(i);
                            if (ImGui::Selectable(u.items[i].c_str(), selected == i)) { u.selectedIndex = i; changed = true; }
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                }
                if (t == UIElementType::Dropdown || t == UIElementType::EnumDropdown) changed |= EditText("Placeholder", u.placeholder);
                if (t == UIElementType::GridView || t == UIElementType::MultiColumnListView) {
                    changed |= ImGui::SliderInt(t == UIElementType::GridView ? "Columns" : "Column count", &u.gridColumns, 1, 12);
                }
                if (IsCollection(t)) {
                    changed |= ImGui::DragFloat(t == UIElementType::GridView ? "Cell height" : "Row height", &u.rowHeight, .5f, 8, 512, "%.0f px");
                    if (t != UIElementType::TreeView) {
                        changed |= ImGui::Checkbox("Virtualize", &u.virtualize);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only build the rows that are scrolled into view");
                        ImGui::SameLine(); changed |= ImGui::Checkbox("Sortable", &u.sortable);
                    }
                }
            }
        });
        if (UsesItems(t)) {
            section("Items", u.items.size() <= 12, [&] {
                ImGui::TextDisabled(t == UIElementType::MultiColumnListView ? "%d rows - separate cells with |" : "%d items",
                                    static_cast<int>(u.items.size()));
                changed |= ItemsEditor(u);
            });
        }
        section("Appearance", false, [&] {
            ImGui::TextDisabled("Alpha 0 derives the color from Tint.");
            changed |= ImGui::ColorEdit4("Accent", &u.fillColor.x, ImGuiColorEditFlags_AlphaPreviewHalf);
            changed |= ImGui::ColorEdit4("Background", &u.backgroundColor.x, ImGuiColorEditFlags_AlphaPreviewHalf);
            changed |= ImGui::ColorEdit4("Border", &u.borderColor.x, ImGuiColorEditFlags_AlphaPreviewHalf);
            changed |= ImGui::ColorEdit4("Text", &u.textColor.x, ImGuiColorEditFlags_AlphaPreviewHalf);
            changed |= ImGui::DragFloat("Corner radius", &u.rounding, .25f, 0, 1024, "%.1f px");
            changed |= ImGui::DragFloat("Border width", &u.borderWidth, .1f, 0, 128, "%.1f px");
        });
        section("Behaviour", false, [&] {
            changed |= ImGui::Checkbox("Interactable", &u.interactable);
            changed |= EditText("Tooltip", u.tooltip, "Shown on hover");
        });
    }
    section("Layout and Clipping", false, [&] {
        ImGui::BeginDisabled(transformLocked);
        changed |= ImGui::Checkbox("Clip children to rectangle", &u.clipChildren);
        int mode = static_cast<int>(u.layout);
        if (ImGui::Combo("Layout", &mode, "None\0Horizontal\0Vertical\0Grid\0")) { u.layout = static_cast<UILayoutMode>(mode); changed = true; }
        if (u.layout != UILayoutMode::None) {
            changed |= ImGui::DragFloat2("Spacing", &u.spacing.x, .5f, 0, 4096);
            changed |= ImGui::DragFloat4("Padding L/T/R/B", &u.padding.x, .5f, 0, 4096);
            if (u.layout == UILayoutMode::Grid) { changed |= ImGui::InputInt("Grid columns", &u.gridColumns); u.gridColumns = std::clamp(u.gridColumns, 1, 1024); }
            changed |= ImGui::Checkbox("Fit content width", &u.fitWidth);
            ImGui::SameLine(); changed |= ImGui::Checkbox("Fit content height", &u.fitHeight);
        }
        changed |= ImGui::DragFloat4("Margin L/T/R/B", &u.margin.x, .5f, 0, 4096);
        changed |= ImGui::DragFloat2("Minimum size", &u.minimumSize.x, 1, 0, 65536);
        changed |= ImGui::DragFloat2("Preferred size (-1 inherits)", &u.preferredSize.x, 1, -1, 65536);
        changed |= ImGui::DragFloat2("Maximum size", &u.maximumSize.x, 1, 0, 65536);
        changed |= ImGui::DragFloat("Flexible weight", &u.flexibleSize, .1f, 0, 1000);
        changed |= ImGui::DragFloat("Aspect ratio (0 disables)", &u.aspectRatio, .01f, 0, 100);
        changed |= ImGui::Checkbox("Responsive anchors", &u.responsive);
        if (u.responsive) {
            changed |= ImGui::DragFloat2("Anchor min", &u.anchorMin.x, .01f, 0, 1);
            changed |= ImGui::DragFloat2("Anchor max", &u.anchorMax.x, .01f, 0, 1);
            changed |= ImGui::DragFloat2("Pivot", &u.pivot.x, .01f, 0, 1);
        }
        ImGui::EndDisabled();
    });
    return changed;
}
} // namespace UIControls
