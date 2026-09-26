#include "../../EditorLocalization.h"
#include "Engine.h"

namespace Loc = Modularity::Loc;

// Dock drawers: the left, right and bottom dock areas fold away into a slim rail.
// Double-click (or double-tap) a drawer's tab strip and the panel bumps out a
// little, then slides into a rail along its edge; clicking or tapping the rail
// slides it back. The rail is drawn with ModuGUI's own tab shading and window
// icons, so it reads as part of the dock rather than an overlay.
namespace {
enum class DockDrawerSide { Left, Right, Bottom };

// Open: untouched, the user resizes it freely. Bump: the short grow before
// closing. Closing/Opening: the slide. Closed: folded into the rail.
enum class DrawerPhase { Open, Bump, Closing, Closed, Opening };

constexpr double kBumpSeconds = 0.10;
constexpr double kCloseSeconds = 0.26;
constexpr double kOpenSeconds = 0.24;

struct DockDrawerTarget {
  ImGuiDockNode *splitParent = nullptr;
  ImGuiDockNode *drawerBranch = nullptr;
  ImGuiDockNode *oppositeBranch = nullptr;
};

struct DockDrawerState {
  ImGuiID activeSplitParentId = 0;
  DrawerPhase phase = DrawerPhase::Open;
  double phaseStart = 0.0;
  float phaseFromExtent = 0.0f;  // drawer size when the current slide began
  float expandedExtent = 0.0f;   // size to open back to
  // Collapsed as of the last slide sound, so each user action is heard once and
  // layout resets that force the drawer open are not.
  bool reportedCollapsed = false;
  // Tab to select once the drawer is open again (rail slot clicks).
  ImGuiID pendingFocusNodeId = 0;
  ImGuiID pendingFocusTabId = 0;
  // Leaf nodes whose tab bar this drawer hid, so only those are shown again.
  ImVector<ImGuiID> hiddenLeafIds;
  // Tap tracking: a tap is a press and release in place, so dragging a tab to
  // undock or reorder it never counts.
  bool pressInside = false;
  ImVec2 pressPos;
  double lastTapTime = -1.0;
  ImVec2 lastTapPos;
};

bool IsCollapsingOrCollapsed(DrawerPhase phase) {
  return phase == DrawerPhase::Bump || phase == DrawerPhase::Closing ||
         phase == DrawerPhase::Closed;
}

float EaseInOutCubic(float t) {
  return t < 0.5f ? 4.0f * t * t * t
                  : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}

float EaseOutCubic(float t) { return 1.0f - std::pow(1.0f - t, 3.0f); }

float DistanceSq(const ImVec2 &a, const ImVec2 &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy;
}

float SmoothStep(float edge0, float edge1, float x) {
  const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Touch gets bigger rail slots and a looser double-tap. Sticky, so the layout
// does not change size every time a mouse event arrives on a hybrid device.
bool TouchPreferred() {
#ifdef __ANDROID__
  return true;
#else
  static bool touch = false;
  const ImGuiIO &io = ImGui::GetIO();
  if (io.MouseClicked[ImGuiMouseButton_Left]) {
    touch = io.MouseSource == ImGuiMouseSource_TouchScreen;
  }
  return touch;
#endif
}

void addRotatedText90CW(ImDrawList *drawList, ImFont *font, float fontSize,
                        const ImRect &bounds, ImU32 color, const char *text,
                        const char *textEnd) {
  if (!drawList || !font || !text || !*text)
    return;
  if ((color & IM_COL32_A_MASK) == 0)
    return;

  ImFontBaked *baked = font->GetFontBaked(fontSize);
  if (!baked)
    return;

  const ImVec2 textSize =
      font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, textEnd, nullptr);
  if (textSize.x <= 0.0f || textSize.y <= 0.0f)
    return;

  // Rotating CW maps original (w,h) text bounds to (h,w).
  const float rotatedWidth = textSize.y;
  const float rotatedHeight = textSize.x;
  const float originX =
      bounds.Min.x + (bounds.GetWidth() - rotatedWidth) * 0.5f;
  const float originY =
      bounds.Min.y + (bounds.GetHeight() - rotatedHeight) * 0.5f;
  const float scale = (baked->Size > 0.0f) ? (fontSize / baked->Size) : 1.0f;

  float cursorX = 0.0f;
  const char *s = text;
  while (s < textEnd && *s) {
    unsigned int c = 0;
    const int bytes = ImTextCharFromUtf8(&c, s, textEnd);
    if (bytes <= 0)
      break;
    s += bytes;

    if (c == '\n' || c == '\r')
      continue;
    ImFontGlyph *glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(c));
    if (!glyph)
      continue;

    const float x1 = cursorX + glyph->X0 * scale;
    const float x2 = cursorX + glyph->X1 * scale;
    const float y1 = glyph->Y0 * scale;
    const float y2 = glyph->Y1 * scale;

    auto rotateCW = [&](float x, float y) -> ImVec2 {
      // (x,y) -> (y, textWidth - x), then centered in target bounds.
      return ImVec2(originX + y, originY + (textSize.x - x));
    };

    drawList->AddImageQuad(ImGui::GetIO().Fonts->TexRef, rotateCW(x1, y1),
                           rotateCW(x2, y1), rotateCW(x2, y2),
                           rotateCW(x1, y2), ImVec2(glyph->U0, glyph->V0),
                           ImVec2(glyph->U1, glyph->V0),
                           ImVec2(glyph->U1, glyph->V1),
                           ImVec2(glyph->U0, glyph->V1), color);

    cursorX += glyph->AdvanceX * scale;
  }
}

void collectLeafNodes(ImGuiDockNode *node, ImVector<ImGuiDockNode *> &out) {
  if (!node)
    return;
  if (node->IsLeafNode()) {
    out.push_back(node);
    return;
  }
  collectLeafNodes(node->ChildNodes[0], out);
  collectLeafNodes(node->ChildNodes[1], out);
}

// Anchors are the panels' stable "###Name" ids, so the drawers keep working
// whatever language the titles are shown in.
DockDrawerTarget findDockDrawerTarget(const char *const *anchorWindows,
                                      int anchorCount, DockDrawerSide side) {
  const ImGuiAxis axis =
      (side == DockDrawerSide::Bottom) ? ImGuiAxis_Y : ImGuiAxis_X;

  for (int anchorIdx = 0; anchorIdx < anchorCount; ++anchorIdx) {
    ImGuiWindow *anchor =
        ImGui::FindWindowByName(Loc::WindowRef(anchorWindows[anchorIdx]));
    if (!anchor || !anchor->DockNode)
      continue;

    ImGuiDockNode *source = anchor->DockNode;
    ImGuiDockNode *current = source;
    while (current && current->ParentNode) {
      ImGuiDockNode *parent = current->ParentNode;
      if (parent->SplitAxis == axis && parent->ChildNodes[0] &&
          parent->ChildNodes[1]) {
        ImGuiDockNode *child0 = parent->ChildNodes[0];
        ImGuiDockNode *child1 = parent->ChildNodes[1];
        const bool sourceInChild0 =
            ImGui::DockNodeIsInHierarchyOf(source, child0);
        const bool sourceInChild1 =
            ImGui::DockNodeIsInHierarchyOf(source, child1);
        if (!sourceInChild0 && !sourceInChild1) {
          current = parent;
          continue;
        }

        ImGuiDockNode *drawerCandidate = sourceInChild0 ? child0 : child1;
        ImGuiDockNode *oppositeCandidate =
            (drawerCandidate == child0) ? child1 : child0;

        bool matchesSide = false;
        if (side == DockDrawerSide::Bottom) {
          matchesSide =
              drawerCandidate->Pos.y >= oppositeCandidate->Pos.y - 0.5f;
        } else if (side == DockDrawerSide::Left) {
          matchesSide =
              drawerCandidate->Pos.x <= oppositeCandidate->Pos.x + 0.5f;
        } else {
          matchesSide =
              drawerCandidate->Pos.x >= oppositeCandidate->Pos.x - 0.5f;
        }
        // Never fold the central node (the viewport) away.
        if (matchesSide &&
            !ImGui::DockNodeIsInHierarchyOf(
                ImGui::DockNodeGetRootNode(drawerCandidate)->CentralNode,
                drawerCandidate)) {
          return DockDrawerTarget{parent, drawerCandidate, oppositeCandidate};
        }
      }
      current = parent;
    }
  }

  return {};
}

// A double tap or double click on one of the drawer's tab strips. Counted on
// release, with a looser distance for fingers.
bool detectDoubleTap(DockDrawerState &state,
                     const ImVector<ImGuiDockNode *> &leaves, bool touch) {
  ImGuiContext &g = *GImGui;
  const ImGuiIO &io = g.IO;
  auto overTabStrip = [&](const ImVec2 &pos) {
    for (ImGuiDockNode *leaf : leaves) {
      ImGuiTabBar *tabBar = leaf->TabBar;
      if (!tabBar || !leaf->HostWindow || leaf->IsHiddenTabBar())
        continue;
      if (!tabBar->BarRect.Contains(pos))
        continue;
      // Only when nothing (a popup, a floating window) sits over the strip.
      if (g.HoveredWindow &&
          g.HoveredWindow->RootWindowDockTree ==
              leaf->HostWindow->RootWindowDockTree) {
        return true;
      }
    }
    return false;
  };

  if (io.MouseClicked[ImGuiMouseButton_Left]) {
    state.pressInside = overTabStrip(io.MousePos);
    state.pressPos = io.MousePos;
  }
  if (!io.MouseReleased[ImGuiMouseButton_Left] || !state.pressInside)
    return false;
  state.pressInside = false;

  const float fontSize = ImGui::GetFontSize();
  const float slop = touch ? fontSize * 1.2f : ImMax(io.MouseDragThreshold, 4.0f);
  if (DistanceSq(io.MousePos, state.pressPos) > slop * slop)
    return false;  // a drag: the tab bar's own undock / reorder

  const double now = ImGui::GetTime();
  const double pairTime =
      touch ? ImMax(0.42, static_cast<double>(io.MouseDoubleClickTime))
            : static_cast<double>(io.MouseDoubleClickTime);
  const float pairDistance =
      touch ? fontSize * 2.4f : ImMax(io.MouseDoubleClickMaxDist, 6.0f);
  if (state.lastTapTime >= 0.0 && now - state.lastTapTime <= pairTime &&
      DistanceSq(io.MousePos, state.lastTapPos) <=
          pairDistance * pairDistance) {
    state.lastTapTime = -1.0;
    return true;
  }
  state.lastTapTime = now;
  state.lastTapPos = io.MousePos;
  return false;
}

void setDrawerTabBarsHidden(DockDrawerState &state,
                            const ImVector<ImGuiDockNode *> &leaves,
                            bool hidden) {
  if (hidden) {
    for (ImGuiDockNode *leaf : leaves) {
      if (leaf->LocalFlags & ImGuiDockNodeFlags_HiddenTabBar)
        continue;
      leaf->SetLocalFlags(leaf->LocalFlags | ImGuiDockNodeFlags_HiddenTabBar);
      state.hiddenLeafIds.push_back(leaf->ID);
    }
    return;
  }
  for (ImGuiID id : state.hiddenLeafIds) {
    if (ImGuiDockNode *node = ImGui::DockBuilderGetNode(id)) {
      node->SetLocalFlags(node->LocalFlags & ~ImGuiDockNodeFlags_HiddenTabBar);
    }
  }
  state.hiddenLeafIds.clear();
}

void startPhase(DockDrawerState &state, DrawerPhase phase, float fromExtent) {
  state.phase = phase;
  state.phaseStart = ImGui::GetTime();
  state.phaseFromExtent = fromExtent;
}

void openDrawer(DockDrawerState &state, float currentExtent,
                ImGuiID focusNodeId = 0, ImGuiID focusTabId = 0) {
  if (focusTabId != 0) {
    state.pendingFocusNodeId = focusNodeId;
    state.pendingFocusTabId = focusTabId;
  }
  if (state.phase == DrawerPhase::Open || state.phase == DrawerPhase::Opening)
    return;
  startPhase(state, DrawerPhase::Opening, currentExtent);
}

void applyPendingTabFocus(DockDrawerState &state) {
  if (state.pendingFocusTabId == 0)
    return;
  ImGuiDockNode *node = ImGui::DockBuilderGetNode(state.pendingFocusNodeId);
  ImGuiTabBar *tabBar = node ? node->TabBar : nullptr;
  if (!tabBar) {
    state.pendingFocusTabId = 0;
    return;
  }
  for (int i = 0; i < tabBar->Tabs.Size; ++i) {
    ImGuiTabItem *tab = &tabBar->Tabs[i];
    if (tab->ID != state.pendingFocusTabId)
      continue;
    if (tabBar->SelectedTabId == tab->ID) {
      state.pendingFocusTabId = 0;
    } else {
      ImGui::TabBarQueueFocus(tabBar, tab);
    }
    return;
  }
  state.pendingFocusTabId = 0;
}

struct RailTab {
  ImGuiDockNode *node = nullptr;
  ImGuiTabItem *tab = nullptr;
  const char *label = nullptr;
  const char *labelEnd = nullptr;  // before the "###Id" part, like ImGui's own tabs
  const ImGuiNamedIcon *icon = nullptr;
  bool selected = false;
  bool groupStart = false;  // first tab of a leaf after another leaf's tabs
};

void collectRailTabs(const ImVector<ImGuiDockNode *> &leaves,
                     ImVector<RailTab> &out) {
  for (ImGuiDockNode *leaf : leaves) {
    ImGuiTabBar *tabBar = leaf->TabBar;
    if (!tabBar)
      continue;
    bool first = true;
    for (int i = 0; i < tabBar->Tabs.Size; ++i) {
      ImGuiTabItem *tab = &tabBar->Tabs[i];
      RailTab entry;
      entry.node = leaf;
      entry.tab = tab;
      entry.label = ImGui::TabBarGetTabName(tabBar, tab);
      entry.labelEnd = ImGui::FindRenderedTextEnd(entry.label);
      entry.icon = ImGui::FindNamedIcon(tab->Window ? tab->Window->Name
                                                     : entry.label);
      entry.selected = tabBar->SelectedTabId == tab->ID ||
                       tabBar->VisibleTabId == tab->ID;
      entry.groupStart = first && !out.empty();
      out.push_back(entry);
      first = false;
    }
  }
}

ImU32 railTabColor(bool selected, bool hovered, bool focused) {
  if (selected)
    return ImGui::GetColorU32(focused ? ImGuiCol_TabSelected
                                      : ImGuiCol_TabDimmedSelected);
  if (hovered)
    return ImGui::GetColorU32(ImGuiCol_TabHovered);
  return ImGui::GetColorU32(focused ? ImGuiCol_Tab : ImGuiCol_TabDimmed);
}

// The folded drawer: a strip of shaded tabs where the panel was. `reveal` fades
// and slides it in while the drawer closes, and out while it opens.
void renderDrawerRail(DockDrawerState &state, const DockDrawerTarget &target,
                      const ImVector<ImGuiDockNode *> &leaves,
                      DockDrawerSide side, float railExtent, float reveal,
                      bool touch) {
  if (reveal <= 0.001f || !target.drawerBranch || !target.splitParent)
    return;
  ImVector<RailTab> tabs;
  collectRailTabs(leaves, tabs);
  if (tabs.empty())
    return;

  const ImGuiStyle &style = ImGui::GetStyle();
  const float fontSize = ImGui::GetFontSize();
  const bool bottom = side == DockDrawerSide::Bottom;
  const ImGuiDockNode *branch = target.drawerBranch;

  // The rail fills the folded drawer's edge; it slides in from that edge.
  const float slide = (1.0f - reveal) * railExtent * 0.5f;
  ImVec2 railPos = branch->Pos;
  ImVec2 railSize = branch->Size;
  if (bottom) {
    railSize.y = railExtent;
    railPos.y = branch->Pos.y + branch->Size.y - railExtent + slide;
  } else {
    railSize.x = railExtent;
    railPos.x = side == DockDrawerSide::Left
                    ? branch->Pos.x - slide
                    : branch->Pos.x + branch->Size.x - railExtent + slide;
  }

  char railWindowName[64];
  std::snprintf(railWindowName, sizeof(railWindowName), "##DockRail_%c_%08X",
                side == DockDrawerSide::Left
                    ? 'L'
                    : (side == DockDrawerSide::Right ? 'R' : 'B'),
                target.splitParent->ID);
  const ImGuiWindowFlags railFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

  ImGui::SetNextWindowPos(railPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(railSize, ImGuiCond_Always);
  if (branch->HostWindow) {
    ImGui::SetNextWindowViewport(branch->HostWindow->ViewportId);
  }
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * reveal);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  if (ImGui::Begin(railWindowName, nullptr, railFlags)) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const ImRect railRect(winPos, ImVec2(winPos.x + winSize.x, winPos.y + winSize.y));

    // The strip itself is a toolbar surface, like a dock node's tab bar area.
    ImGui::ShadeRect(draw, railRect.Min, railRect.Max,
                     ImGui::GetColorU32(ImGuiCol_MenuBarBg),
                     ImGuiShadeClass_MenuBar, ImGuiShadeState_Normal);
    const ImU32 edgeCol = ImGui::GetColorU32(ImGuiCol_Separator);
    if (bottom) {
      draw->AddLine(railRect.Min, ImVec2(railRect.Max.x, railRect.Min.y),
                    edgeCol);
    } else if (side == DockDrawerSide::Left) {
      draw->AddLine(ImVec2(railRect.Max.x - 1.0f, railRect.Min.y),
                    ImVec2(railRect.Max.x - 1.0f, railRect.Max.y), edgeCol);
    } else {
      draw->AddLine(railRect.Min, ImVec2(railRect.Min.x, railRect.Max.y),
                    edgeCol);
    }

    // Tapping empty rail opens the drawer on whatever tab was showing.
    ImGui::SetCursorScreenPos(railRect.Min);
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##RailBackground", railRect.GetSize())) {
      openDrawer(state, railExtent);
    }

    const float inset = ImFloor(fontSize * (touch ? 0.22f : 0.16f));
    const float gap = ImMax(2.0f, style.ItemInnerSpacing.y);
    const float groupGap = ImFloor(fontSize * 0.55f);
    const float iconSize = ImFloor(fontSize);
    const float pad = style.FramePadding.x;
    float cursor = (bottom ? railRect.Min.x : railRect.Min.y) + inset + 1.0f;

    for (const RailTab &entry : tabs) {
      const bool focused =
          (entry.node->TabBar->Flags & ImGuiTabBarFlags_IsFocused) != 0;
      if (entry.groupStart) {
        const float mid = cursor + groupGap * 0.5f - gap * 0.5f;
        if (bottom) {
          draw->AddLine(ImVec2(mid, railRect.Min.y + inset * 2.0f),
                        ImVec2(mid, railRect.Max.y - inset * 2.0f), edgeCol);
        } else {
          draw->AddLine(ImVec2(railRect.Min.x + inset * 2.0f, mid),
                        ImVec2(railRect.Max.x - inset * 2.0f, mid), edgeCol);
        }
        cursor += groupGap;
      }

      const ImVec2 labelSize = ImGui::CalcTextSize(entry.label, entry.labelEnd);
      const float iconSpan = entry.icon ? iconSize + style.ItemInnerSpacing.x : 0.0f;
      ImRect slot;
      if (bottom) {
        const float width = ImMin(labelSize.x + iconSpan + pad * 2.0f,
                                  fontSize * 12.0f);
        slot = ImRect(ImVec2(cursor, railRect.Min.y + inset + 1.0f),
                      ImVec2(cursor + width, railRect.Max.y));
      } else {
        const float minHeight = fontSize * (touch ? 3.2f : 2.6f);
        const float height = std::clamp(labelSize.x + iconSpan + pad * 2.0f,
                                        minHeight, fontSize * 9.0f);
        const float x0 = railRect.Min.x + (side == DockDrawerSide::Left ? 0.0f : inset + 1.0f);
        const float x1 = railRect.Max.x - (side == DockDrawerSide::Left ? inset + 1.0f : 0.0f);
        slot = ImRect(ImVec2(x0, cursor), ImVec2(x1, cursor + height));
      }
      if ((bottom ? slot.Min.x : slot.Min.y) >
          (bottom ? railRect.Max.x : railRect.Max.y) - 4.0f) {
        break;  // out of room; the rest show once the drawer opens
      }

      ImGui::PushID(static_cast<int>(entry.tab->ID));
      ImGui::SetCursorScreenPos(slot.Min);
      const bool pressed = ImGui::InvisibleButton("##RailTab", slot.GetSize());
      ImGui::ReportItemHoverFeedback();
      const bool hovered = ImGui::IsItemHovered();
      const bool held = ImGui::IsItemActive();
      if (pressed) {
        openDrawer(state, railExtent, entry.node->ID, entry.tab->ID);
      }
      if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetItemTooltip("%.*s", static_cast<int>(entry.labelEnd - entry.label), entry.label);
      }

      const ImU32 fill = railTabColor(entry.selected, hovered, focused);
      const ImGuiShadeClass shadeClass =
          entry.selected ? ImGuiShadeClass_TabActive : ImGuiShadeClass_Tab;
      const ImGuiShadeState shadeState =
          ImGui::ShadeStateFromInteraction(hovered, held, entry.selected);
      const ImU32 overline = ImGui::GetColorU32(
          focused ? ImGuiCol_TabSelectedOverline
                  : ImGuiCol_TabDimmedSelectedOverline);
      const float overlineSize = ImMax(1.0f, style.TabBarOverlineSize);
      const ImU32 textCol = ImGui::GetColorU32(
          (entry.selected || hovered) ? ImGuiCol_Text : ImGuiCol_TextDisabled);

      if (bottom) {
        // Exactly a dock tab: same shape, shading and overline.
        ImGui::TabItemBackground(draw, slot, ImGuiTabItemFlags_None, fill,
                                 shadeClass, shadeState);
        if (entry.selected) {
          draw->AddRectFilled(ImVec2(slot.Min.x + style.TabRounding, slot.Min.y),
                              ImVec2(slot.Max.x - style.TabRounding,
                                     slot.Min.y + overlineSize),
                              overline);
        }
        float textX = slot.Min.x + pad;
        if (entry.icon) {
          const ImVec2 iconMin(textX, slot.GetCenter().y - iconSize * 0.5f);
          draw->AddImage(entry.icon->TexId, iconMin,
                         ImVec2(iconMin.x + iconSize, iconMin.y + iconSize), entry.icon->Uv0,
                         entry.icon->Uv1,
                         ImGui::GetColorU32(ImVec4(1, 1, 1, entry.selected ? 1.0f : 0.72f)));
          textX += iconSpan;
        }
        draw->PushClipRect(slot.Min, ImVec2(slot.Max.x - pad * 0.5f, slot.Max.y), true);
        draw->AddText(ImVec2(textX, slot.GetCenter().y - labelSize.y * 0.5f),
                      textCol, entry.label, entry.labelEnd);
        draw->PopClipRect();
      } else {
        // A dock tab turned on its side: rounded on the side facing the
        // viewport, overline on that side when selected.
        const bool left = side == DockDrawerSide::Left;
        const ImDrawFlags corners =
            left ? (ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight)
                 : (ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft);
        ImGui::ShadeRect(draw, slot.Min, slot.Max, fill, shadeClass, shadeState,
                         style.TabRounding, corners);
        if (entry.selected) {
          const float x = left ? slot.Max.x - overlineSize : slot.Min.x;
          draw->AddRectFilled(ImVec2(x, slot.Min.y + style.TabRounding),
                              ImVec2(x + overlineSize, slot.Max.y - style.TabRounding),
                              overline);
        }
        float textTop = slot.Min.y + pad;
        if (entry.icon) {
          const ImVec2 iconMin(slot.GetCenter().x - iconSize * 0.5f, textTop);
          draw->AddImage(entry.icon->TexId, iconMin,
                         ImVec2(iconMin.x + iconSize, iconMin.y + iconSize), entry.icon->Uv0,
                         entry.icon->Uv1,
                         ImGui::GetColorU32(ImVec4(1, 1, 1, entry.selected ? 1.0f : 0.72f)));
          textTop += iconSpan;
        }
        const ImRect textRect(ImVec2(slot.Min.x, textTop),
                              ImVec2(slot.Max.x, slot.Max.y - pad));
        draw->PushClipRect(textRect.Min, textRect.Max, true);
        addRotatedText90CW(draw, ImGui::GetFont(), fontSize, textRect, textCol,
                           entry.label, entry.labelEnd);
        draw->PopClipRect();
      }
      ImGui::PopID();
      cursor = (bottom ? slot.Max.x : slot.Max.y) + gap;
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(4);
}

void updateDockDrawer(DockDrawerState &state, const DockDrawerTarget &target,
                      DockDrawerSide side, bool touch) {
  if (!target.splitParent || !target.drawerBranch || !target.oppositeBranch) {
    setDrawerTabBarsHidden(state, {}, false);
    state = DockDrawerState();
    return;
  }

  const bool bottom = side == DockDrawerSide::Bottom;
  const float fontSize = ImGui::GetFontSize();
  const float collapsedExtent =
      bottom ? ImGui::GetFrameHeight() + ImFloor(fontSize * (touch ? 0.9f : 0.5f))
             : ImFloor(fontSize * (touch ? 2.6f : 2.0f));
  auto extentOf = [&](const ImGuiDockNode *node) {
    return bottom ? node->Size.y : node->Size.x;
  };
  const float liveExtent = extentOf(target.drawerBranch);
  const float totalExtent = ImMax(0.0f, extentOf(target.splitParent));
  const float minOppositeExtent = bottom ? 96.0f : 220.0f;
  const float maxDrawerExtent =
      ImMax(collapsedExtent, totalExtent - minOppositeExtent);
  const float defaultExtent = std::clamp(totalExtent * (bottom ? 0.30f : 0.22f),
                                         collapsedExtent + 40.0f, maxDrawerExtent);

  ImVector<ImGuiDockNode *> leaves;
  collectLeafNodes(target.drawerBranch, leaves);

  if (state.activeSplitParentId != target.splitParent->ID) {
    setDrawerTabBarsHidden(state, {}, false);
    state = DockDrawerState();
    state.activeSplitParentId = target.splitParent->ID;
    state.expandedExtent = liveExtent;
    // A layout saved while a drawer was folded comes back tiny, sometimes with
    // its tab bar hidden. Resume it as a rail so it can be opened again.
    if (liveExtent > 1.0f && liveExtent <= collapsedExtent + 4.0f) {
      for (ImGuiDockNode *leaf : leaves) {
        if (leaf->LocalFlags & ImGuiDockNodeFlags_HiddenTabBar)
          state.hiddenLeafIds.push_back(leaf->ID);
      }
      state.expandedExtent = defaultExtent;
      state.phase = DrawerPhase::Closed;
      state.reportedCollapsed = true;
    }
  }

  // Double tap / double click the tab strip: fold (or fold back mid-open).
  if (detectDoubleTap(state, leaves, touch) &&
      (state.phase == DrawerPhase::Open || state.phase == DrawerPhase::Opening)) {
    if (state.phase == DrawerPhase::Open && liveExtent > collapsedExtent + 8.0f) {
      state.expandedExtent = liveExtent;
    }
    startPhase(state, DrawerPhase::Bump, liveExtent);
  }

  const bool collapsed = IsCollapsingOrCollapsed(state.phase);
  if (collapsed != state.reportedCollapsed) {
    state.reportedCollapsed = collapsed;
    ImGui::ReportWidgetFeedback(collapsed ? ImGuiWidgetFeedback_SlideClose
                                          : ImGuiWidgetFeedback_SlideOpen);
  }

  const float expanded =
      std::clamp(state.expandedExtent > collapsedExtent + 8.0f ? state.expandedExtent
                                                               : defaultExtent,
                 collapsedExtent + 8.0f, maxDrawerExtent);
  const float elapsed = static_cast<float>(ImGui::GetTime() - state.phaseStart);
  // Decided before the switch: the frame an opening finishes still sets the final size.
  const bool driveSize = state.phase != DrawerPhase::Open;
  float drawerExtent = liveExtent;
  float railReveal = 0.0f;
  switch (state.phase) {
  case DrawerPhase::Open:
    break;
  case DrawerPhase::Bump: {
    // The little grow before it goes, so the fold reads as a response to the tap.
    const float t = std::clamp(elapsed / static_cast<float>(kBumpSeconds), 0.0f, 1.0f);
    const float bump = std::clamp(expanded * 0.06f, 8.0f, fontSize * 2.0f);
    drawerExtent = state.phaseFromExtent + bump * std::sin(t * IM_PI);
    if (t >= 1.0f)
      startPhase(state, DrawerPhase::Closing, state.phaseFromExtent);
    break;
  }
  case DrawerPhase::Closing: {
    const float t = std::clamp(elapsed / static_cast<float>(kCloseSeconds), 0.0f, 1.0f);
    drawerExtent = ImLerp(state.phaseFromExtent, collapsedExtent, EaseInOutCubic(t));
    railReveal = SmoothStep(0.55f, 1.0f, t);
    if (t >= 1.0f)
      state.phase = DrawerPhase::Closed;
    break;
  }
  case DrawerPhase::Closed:
    drawerExtent = collapsedExtent;
    railReveal = 1.0f;
    break;
  case DrawerPhase::Opening: {
    const float t = std::clamp(elapsed / static_cast<float>(kOpenSeconds), 0.0f, 1.0f);
    drawerExtent = ImLerp(state.phaseFromExtent, expanded, EaseOutCubic(t));
    railReveal = 1.0f - SmoothStep(0.0f, 0.4f, t);
    if (t >= 1.0f) {
      state.phase = DrawerPhase::Open;
      drawerExtent = expanded;
    }
    break;
  }
  }

  // Open drawers are left to ImGui, so dragging the splitter is never fought.
  if (driveSize) {
    drawerExtent = std::clamp(drawerExtent, collapsedExtent, maxDrawerExtent);
    const float oppositeExtent = ImMax(minOppositeExtent, totalExtent - drawerExtent);
    target.splitParent->AuthorityForSize = ImGuiDataAuthority_DockNode;
    target.drawerBranch->AuthorityForSize = ImGuiDataAuthority_DockNode;
    target.oppositeBranch->AuthorityForSize = ImGuiDataAuthority_DockNode;
    if (bottom) {
      target.drawerBranch->Size.y = target.drawerBranch->SizeRef.y = drawerExtent;
      target.oppositeBranch->Size.y = target.oppositeBranch->SizeRef.y = oppositeExtent;
    } else {
      target.drawerBranch->Size.x = target.drawerBranch->SizeRef.x = drawerExtent;
      target.oppositeBranch->Size.x = target.oppositeBranch->SizeRef.x = oppositeExtent;
    }
  }

  // Tab strips hide once the rail starts to show, and return as it fades.
  setDrawerTabBarsHidden(state, leaves, railReveal > 0.01f);
  if (state.phase == DrawerPhase::Open) {
    applyPendingTabFocus(state);
  }
  renderDrawerRail(state, target, leaves, side, collapsedExtent, railReveal, touch);
}

void updateDockDrawers() {
  static int lastUpdatedFrame = -1;
  const int currentFrame = ImGui::GetFrameCount();
  if (lastUpdatedFrame == currentFrame)
    return;
  lastUpdatedFrame = currentFrame;

  static DockDrawerState leftState;
  static DockDrawerState rightState;
  static DockDrawerState bottomState;

  static const char *kLeftAnchors[] = {"Hierarchy", "Camera"};
  static const char *kRightAnchors[] = {"Inspector", "Environment"};
  static const char *kBottomAnchors[] = {"Project", "Project Settings",
                                         "Animation", "AI Pathfinding",
                                         "Version Control"};

  const bool touch = TouchPreferred();
  updateDockDrawer(leftState,
                   findDockDrawerTarget(kLeftAnchors, IM_ARRAYSIZE(kLeftAnchors),
                                        DockDrawerSide::Left),
                   DockDrawerSide::Left, touch);
  updateDockDrawer(rightState,
                   findDockDrawerTarget(kRightAnchors, IM_ARRAYSIZE(kRightAnchors),
                                        DockDrawerSide::Right),
                   DockDrawerSide::Right, touch);
  updateDockDrawer(bottomState,
                   findDockDrawerTarget(kBottomAnchors, IM_ARRAYSIZE(kBottomAnchors),
                                        DockDrawerSide::Bottom),
                   DockDrawerSide::Bottom, touch);
}
} // namespace

void Engine::updateDockDrawerInteractions() {
  if (pendingWorkspaceReload || workspaceLayoutDirty ||
      glfwGetTime() < workspaceLayoutStabilizeUntil) {
    return;
  }
  updateDockDrawers();
}
