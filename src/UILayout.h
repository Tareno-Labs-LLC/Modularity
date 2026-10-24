#pragma once

#include "SceneObject.h"
#include <unordered_map>

// Per-viewport layout cache. Authored positions/sizes are never overwritten.
// Measure bottom-up, arrange top-down in the existing transform resolver.
namespace UILayout {
// Gap between a Split View's two panes; UIControls draws the drag handle inside it.
inline constexpr float kSplitterThickness = 6.0f;
struct Placement {
    glm::vec2 size;
    glm::vec2 position;
    UIAnchor anchor;
};
class Context {
    const std::vector<SceneObject>& objects;
    std::unordered_map<int, const SceneObject*> nodes;
    std::unordered_map<int, std::vector<const SceneObject*>> children;
    mutable std::unordered_map<int, glm::vec2> measured;
    mutable std::unordered_set<int> measuring;
public:
    explicit Context(std::vector<SceneObject>& scene) : objects(scene) {
        for (const auto& obj : objects) {
            if (!obj.hasUI) continue;
            nodes[obj.id] = &obj;
            if (obj.enabled) children[obj.parentId].push_back(&obj);
        }
        for (auto& obj : scene) {
            if (!obj.hasUI || obj.ui.type != UIElementType::ScrollView) continue;
            auto it = children.find(obj.id);
            if (it == children.end()) continue;
            glm::vec2 extent(0);
            for (auto* child : it->second) extent = glm::max(extent, child->ui.position + Measure(*child));
            obj.ui.contentSize = extent;
            obj.ui.scrollOffset = glm::clamp(obj.ui.scrollOffset, glm::vec2(0), glm::max(glm::vec2(0), extent - obj.ui.size));
        }
    }
    const SceneObject* Find(int id) const {
        auto it = nodes.find(id); return it == nodes.end() ? nullptr : it->second;
    }
    bool UsesLayout(const SceneObject& obj) const {
        const SceneObject* node = &obj;
        for (size_t depth = 0; node && depth <= nodes.size(); ++depth) {
            const auto& u = node->ui;
            if (IsExtendedUIType(u.type) || u.layout != UILayoutMode::None || u.responsive || u.fitWidth || u.fitHeight ||
                u.aspectRatio > 0 || u.preferredSize != glm::vec2(-1) || u.minimumSize != glm::vec2(0) || u.maximumSize != glm::vec2(65536)) return true;
            node = Find(node->parentId);
        }
        return false;
    }
    glm::vec2 Measure(const SceneObject& obj) const {
        auto cached = measured.find(obj.id);
        if (cached != measured.end()) return cached->second;
        if (!measuring.insert(obj.id).second) return obj.ui.size; // corrupt hierarchy
        const auto& u = obj.ui;
        glm::vec2 size = u.size;
        for (int a = 0; a < 2; ++a)
            if (u.preferredSize[a] >= 0.0f) size[a] = u.preferredSize[a];
        auto it = children.find(obj.id);
        glm::vec2 content(0.0f);
        if (it != children.end() && u.layout != UILayoutMode::None) {
            int index = 0;
            glm::vec2 cell(0.0f);
            for (auto* child : it->second) {
                const auto& m = child->ui.margin;
                glm::vec2 childSize = Measure(*child) + glm::vec2(m.x + m.z, m.y + m.w);
                if (u.layout == UILayoutMode::Horizontal) {
                    content.x += childSize.x + (index ? u.spacing.x : 0.0f);
                    content.y = std::max(content.y, childSize.y);
                } else if (u.layout == UILayoutMode::Vertical) {
                    content.y += childSize.y + (index ? u.spacing.y : 0.0f);
                    content.x = std::max(content.x, childSize.x);
                } else cell = glm::max(cell, childSize);
                ++index;
            }
            if (u.layout == UILayoutMode::Grid && index) {
                int cols = std::min(index, std::max(1, u.gridColumns));
                int rows = (index + cols - 1) / cols;
                content = cell * glm::vec2(cols, rows) + u.spacing * glm::vec2(cols - 1, rows - 1);
            }
        }
        content += glm::vec2(u.padding.x + u.padding.z, u.padding.y + u.padding.w);
        if (u.fitWidth) size.x = content.x;
        if (u.fitHeight) size.y = content.y;
        if (u.aspectRatio > 0.0f) size.y = size.x / u.aspectRatio;
        size = glm::clamp(size, glm::max(glm::vec2(0.01f), u.minimumSize),
                          glm::max(glm::max(glm::vec2(0.01f), u.minimumSize), u.maximumSize));
        measuring.erase(obj.id);
        measured[obj.id] = size;
        return size;
    }
    Placement Resolve(const SceneObject& obj, glm::vec2 available, glm::vec2 legacySize) const {
        const auto& u = obj.ui;
        const bool constrained = u.preferredSize.x >= 0 || u.preferredSize.y >= 0 ||
            u.fitWidth || u.fitHeight || u.aspectRatio > 0 ||
            u.minimumSize != glm::vec2(0) || u.maximumSize != glm::vec2(65536);
        Placement p{constrained ? Measure(obj) : legacySize, u.position, u.anchor};
        if (u.responsive) {
            glm::vec2 lo = glm::clamp(u.anchorMin, glm::vec2(0), glm::vec2(1));
            glm::vec2 hi = glm::max(lo, glm::clamp(u.anchorMax, glm::vec2(0), glm::vec2(1)));
            glm::vec2 extent = available * (hi - lo);
            for (int a = 0; a < 2; ++a) if (hi[a] > lo[a]) p.size[a] = extent[a] + u.size[a];
            p.size = glm::clamp(p.size, glm::max(glm::vec2(.01f), u.minimumSize),
                              glm::max(glm::max(glm::vec2(.01f), u.minimumSize), u.maximumSize));
            p.position = available * lo + extent * u.pivot + u.position - p.size * u.pivot;
            p.anchor = UIAnchor::TopLeft;
        }
        auto parentIt = nodes.find(obj.parentId);
        if (parentIt == nodes.end() || !parentIt->second->hasUI) return p;
        const auto& parent = parentIt->second->ui;
        if (parent.type == UIElementType::ScrollView) p.position -= parent.scrollOffset;
        if (parent.type == UIElementType::SplitView) {
            auto siblings = children.find(obj.parentId);
            if (siblings != children.end()) {
                auto it = std::find(siblings->second.begin(), siblings->second.end(), &obj);
                int index = static_cast<int>(it - siblings->second.begin());
                int axis = parent.vertical ? 1 : 0;
                float fraction = std::clamp(parent.sliderValue, .05f, .95f);
                float usable = std::max(0.0f, available[axis] - kSplitterThickness);
                p.anchor = UIAnchor::TopLeft; p.position = glm::vec2(0); p.size = available;
                p.size[axis] = std::max(1.0f, usable * (index == 0 ? fraction : 1 - fraction));
                if (index > 0) p.position[axis] = usable * fraction + kSplitterThickness;
                return p;
            }
        }
        if (parent.layout == UILayoutMode::None) return p;
        auto siblings = children.find(obj.parentId);
        if (siblings == children.end()) return p;
        const auto& list = siblings->second;
        glm::vec2 inner = glm::max(glm::vec2(0), available -
            glm::vec2(parent.padding.x + parent.padding.z, parent.padding.y + parent.padding.w));
        glm::vec2 cursor(parent.padding.x, parent.padding.y);
        glm::vec2 cell(0);
        float used = 0, flex = 0;
        int axis = parent.layout == UILayoutMode::Horizontal ? 0 : 1;
        int index = 0, target = 0;
        for (auto* child : list) {
            glm::vec2 s = Measure(*child);
            const auto& m = child->ui.margin;
            cell = glm::max(cell, s + glm::vec2(m.x + m.z, m.y + m.w));
            used += s[axis] + (axis == 0 ? m.x + m.z : m.y + m.w);
            flex += std::max(0.0f, child->ui.flexibleSize);
            if (child->id == obj.id) target = index;
            ++index;
        }
        used += std::max(0, index - 1) * parent.spacing[axis];
        if (parent.layout == UILayoutMode::Grid) {
            int cols = std::max(1, parent.gridColumns);
            cursor += (cell + parent.spacing) * glm::vec2(target % cols, target / cols);
        } else {
            float extra = std::max(0.0f, inner[axis] - used);
            for (auto* child : list) {
                glm::vec2 s = Measure(*child);
                if (flex > 0) s[axis] += extra * std::max(0.0f, child->ui.flexibleSize) / flex;
                s = glm::min(s, glm::max(child->ui.minimumSize, child->ui.maximumSize));
                if (child->id == obj.id) { p.size = s; break; }
                const auto& m = child->ui.margin;
                cursor[axis] += s[axis] + (axis == 0 ? m.x + m.z : m.y + m.w) + parent.spacing[axis];
            }
        }
        p.position = cursor + glm::vec2(u.margin.x, u.margin.y);
        if (parent.type == UIElementType::ScrollView) p.position -= parent.scrollOffset;
        p.anchor = UIAnchor::TopLeft;
        return p;
    }
};
// Convert an opted-in UI branch into screen rectangles in 2D world canvases.
// The legacy resolver remains authoritative for branches without layout settings.
template<class Vec2, class LegacyResolver, class SizeResolver>
bool ResolveWorldRect(const Context& layout, const SceneObject& obj, Vec2& outMin, Vec2& outMax,
                      LegacyResolver& legacy, SizeResolver sizeOf, int depth = 0) {
    if (depth > 128 || !legacy(obj, outMin, outMax)) return false;
    const SceneObject* parent = layout.Find(obj.parentId);
    if (!parent || !layout.UsesLayout(obj)) return true;
    Vec2 parentMin, parentMax;
    if (!ResolveWorldRect(layout, *parent, parentMin, parentMax, legacy, sizeOf, depth + 1)) return false;
    const glm::vec2 available = layout.Measure(*parent);
    auto p = layout.Resolve(obj, available, sizeOf(obj));
    glm::vec2 scale((parentMax.x - parentMin.x) / std::max(.01f, available.x),
                    (parentMax.y - parentMin.y) / std::max(.01f, available.y));
    glm::vec2 anchor(.5f);
    if (p.anchor == UIAnchor::TopLeft) anchor = glm::vec2(0);
    if (p.anchor == UIAnchor::TopRight) anchor = glm::vec2(1, 0);
    if (p.anchor == UIAnchor::BottomLeft) anchor = glm::vec2(0, 1);
    if (p.anchor == UIAnchor::BottomRight) anchor = glm::vec2(1);
    const glm::vec2 origin = (available * anchor + p.position - p.size * anchor) * scale;
    outMin = Vec2(parentMin.x + origin.x, parentMin.y + origin.y);
    outMax = Vec2(outMin.x + p.size.x * scale.x, outMin.y + p.size.y * scale.y);
    return true;
}
} // namespace UILayout
