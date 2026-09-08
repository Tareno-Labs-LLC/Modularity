#include "Engine.h"

namespace {
enum class DockDrawerSide { Left, Right, Bottom };

struct DockDrawerTarget {
  ImGuiDockNode *splitParent = nullptr;
  ImGuiDockNode *drawerBranch = nullptr;
  ImGuiDockNode *oppositeBranch = nullptr;
};

struct DockDrawerState {
  ImGuiID activeSplitParentId = 0;
  bool collapsed = false;
  float openAmount = 1.0f;
  float expandedExtent = 0.0f;
  ImGuiID pendingTabFocusId = 0;
};

void addRotatedText90CW(ImDrawList *drawList, ImFont *font, float fontSize,
                        const ImRect &bounds, ImU32 color, const char *text) {
  if (!drawList || !font || !text || !*text)
    return;
  if ((color & IM_COL32_A_MASK) == 0)
    return;

  ImFontBaked *baked = font->GetFontBaked(fontSize);
  if (!baked)
    return;

  const ImVec2 textSize =
      font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, nullptr, nullptr);
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
  while (s && *s) {
    unsigned int c = 0;
    const int bytes = ImTextCharFromUtf8(&c, s, nullptr);
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
    const float u1 = glyph->U0;
    const float v1 = glyph->V0;
    const float u2 = glyph->U1;
    const float v2 = glyph->V1;

    auto rotateCW = [&](float x, float y) -> ImVec2 {
      // (x,y) -> (y, textWidth - x), then centered in target bounds.
      return ImVec2(originX + y, originY + (textSize.x - x));
    };

    const ImVec2 pTL = rotateCW(x1, y1);
    const ImVec2 pTR = rotateCW(x2, y1);
    const ImVec2 pBR = rotateCW(x2, y2);
    const ImVec2 pBL = rotateCW(x1, y2);

    drawList->AddImageQuad(ImGui::GetIO().Fonts->TexRef, pTL, pTR, pBR, pBL,
                           ImVec2(u1, v1), ImVec2(u2, v1), ImVec2(u2, v2),
                           ImVec2(u1, v2), color);

    cursorX += glyph->AdvanceX * scale;
  }
}

struct DockTabInteractionState {
  bool hovered = false;
  bool clicked = false;
  bool doubleClicked = false;
};

bool matchesVisibleWindowTitle(const char *windowName,
                               const char *expectedTitle) {
  if (!windowName || !expectedTitle)
    return false;
  const char *idSep = std::strstr(windowName, "###");
  if (!idSep) {
    idSep = std::strstr(windowName, "##");
  }
  const size_t visibleLen =
      idSep ? static_cast<size_t>(idSep - windowName) : std::strlen(windowName);
  return std::strlen(expectedTitle) == visibleLen &&
         std::strncmp(windowName, expectedTitle, visibleLen) == 0;
}

ImGuiWindow *findWindowByVisibleTitle(const char *expectedTitle) {
  if (!expectedTitle || !*expectedTitle)
    return nullptr;
  if (ImGuiWindow *exact = ImGui::FindWindowByName(expectedTitle)) {
    return exact;
  }

  ImGuiContext *ctx = ImGui::GetCurrentContext();
  if (!ctx)
    return nullptr;
  for (ImGuiWindow *window : ctx->Windows) {
    if (!window)
      continue;
    if ((window->Flags & ImGuiWindowFlags_ChildWindow) != 0)
      continue;
    if (!window->DockNode)
      continue;
    if (window && matchesVisibleWindowTitle(window->Name, expectedTitle)) {
      return window;
    }
  }
  return nullptr;
}

DockTabInteractionState
queryDockTabInteraction(const DockDrawerTarget &target,
                        const char *const *anchorWindows, int anchorCount) {
  DockTabInteractionState out;

  if (ImGuiTabBar *tabBar =
          target.drawerBranch ? target.drawerBranch->TabBar : nullptr) {
    if (ImGui::IsMouseHoveringRect(tabBar->BarRect.Min, tabBar->BarRect.Max,
                                   false)) {
      out.hovered = true;
    }
  }

  for (int i = 0; i < anchorCount; ++i) {
    ImGuiWindow *window = findWindowByVisibleTitle(anchorWindows[i]);
    if (!window)
      continue;
    ImRect tabRect = window->DC.DockTabItemRect;
    if (tabRect.GetWidth() <= 0.0f || tabRect.GetHeight() <= 0.0f)
      continue;
    if (ImGui::IsMouseHoveringRect(tabRect.Min, tabRect.Max, false)) {
      out.hovered = true;
      break;
    }
  }

  if (!out.hovered && target.drawerBranch) {
    const ImVec2 headerMin = target.drawerBranch->Pos;
    const ImVec2 headerMax(
        target.drawerBranch->Pos.x + target.drawerBranch->Size.x,
        target.drawerBranch->Pos.y + ImGui::GetFrameHeight() + 8.0f);
    if (ImGui::IsMouseHoveringRect(headerMin, headerMax, false)) {
      out.hovered = true;
    }
  }

  if (out.hovered) {
    ImGuiIO &io = ImGui::GetIO();
    out.doubleClicked = io.MouseClicked[ImGuiMouseButton_Left] &&
                        io.MouseClickedCount[ImGuiMouseButton_Left] >= 2;
    out.clicked =
        !out.doubleClicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }
  return out;
}

void queueDrawerTabFocus(DockDrawerState &state, ImGuiTabBar *tabBar,
                         ImGuiID tabId) {
  if (!tabBar || tabId == 0)
    return;
  for (int i = 0; i < tabBar->Tabs.Size; ++i) {
    ImGuiTabItem *tab = &tabBar->Tabs[i];
    if (tab->ID != tabId)
      continue;
    ImGui::TabBarQueueFocus(tabBar, tab);
    state.pendingTabFocusId = tabId;
    return;
  }
}

void applyPendingDrawerTabFocus(DockDrawerState &state, ImGuiTabBar *tabBar) {
  if (!tabBar || state.pendingTabFocusId == 0)
    return;

  bool found = false;
  for (int i = 0; i < tabBar->Tabs.Size; ++i) {
    ImGuiTabItem *tab = &tabBar->Tabs[i];
    if (tab->ID != state.pendingTabFocusId)
      continue;
    found = true;
    if (tabBar->SelectedTabId == tab->ID || tabBar->VisibleTabId == tab->ID) {
      state.pendingTabFocusId = 0;
      return;
    }
    ImGui::TabBarQueueFocus(tabBar, tab);
    break;
  }

  if (!found) {
    state.pendingTabFocusId = 0;
  }
}

void drawCollapsedDrawerHandleGrip(ImDrawList *draw, const ImRect &rect,
                                   DockDrawerSide side, ImU32 color) {
  if (!draw || (color & IM_COL32_A_MASK) == 0)
    return;

  if (side == DockDrawerSide::Bottom) {
    const float gripWidth = ImMin(16.0f, ImMax(8.0f, rect.GetWidth() - 18.0f));
    const float x0 = rect.GetCenter().x - gripWidth * 0.5f;
    const float x1 = x0 + gripWidth;
    const float startY = rect.Min.y + 5.0f;
    for (int i = 0; i < 2; ++i) {
      const float y = startY + static_cast<float>(i) * 3.0f;
      draw->AddLine(ImVec2(x0, y), ImVec2(x1, y), color, 1.25f);
    }
    return;
  }

  const float gripX = (side == DockDrawerSide::Left) ? (rect.Max.x - 5.0f)
                                                     : (rect.Min.x + 5.0f);
  const float centerY = rect.GetCenter().y;
  for (int i = -1; i <= 1; ++i) {
    const float y0 = centerY + static_cast<float>(i) * 4.0f - 1.4f;
    const float y1 = y0 + 2.8f;
    draw->AddLine(ImVec2(gripX, y0), ImVec2(gripX, y1), color, 1.2f);
  }
}

void renderCollapsedSideDockRail(DockDrawerState &state,
                                 const DockDrawerTarget &target,
                                 DockDrawerSide side, float railWidth,
                                 float revealAmount) {
  if (side == DockDrawerSide::Bottom)
    return;
  if (!target.drawerBranch || !target.splitParent)
    return;
  ImGuiTabBar *tabBar = target.drawerBranch->TabBar;
  if (!tabBar || tabBar->Tabs.Size <= 0)
    return;
  const float reveal = std::clamp(revealAmount, 0.0f, 1.0f);
  if (reveal <= 0.001f)
    return;

  const float splitMinX = target.splitParent->Pos.x;
  const float splitMaxX =
      target.splitParent->Pos.x + target.splitParent->Size.x;
  const float splitWidth = ImMax(1.0f, splitMaxX - splitMinX);
  const float fullRailWidth =
      std::clamp(ImMax(railWidth, 22.0f), 8.0f, splitWidth);
  const float visibleRailWidth = ImMax(1.0f, fullRailWidth * reveal);

  const float branchMinX = target.drawerBranch->Pos.x;
  const float branchMaxX =
      target.drawerBranch->Pos.x + target.drawerBranch->Size.x;
  const float branchMinY = target.drawerBranch->Pos.y;
  const float branchMaxY =
      target.drawerBranch->Pos.y + target.drawerBranch->Size.y;

  ImVec2 railPos(branchMinX, branchMinY);
  ImVec2 railSize = target.drawerBranch->Size;
  railSize.x = visibleRailWidth;

  const bool hasValidBarRect =
      tabBar->BarRect.GetWidth() > 1.0f && tabBar->BarRect.GetHeight() > 1.0f;
  const float hingeX = [&]() {
    if (side == DockDrawerSide::Left) {
      const float preferred =
          hasValidBarRect ? tabBar->BarRect.Max.x : branchMaxX;
      return std::clamp(preferred, splitMinX, splitMaxX);
    }
    const float preferred =
        hasValidBarRect ? tabBar->BarRect.Min.x : branchMinX;
    return std::clamp(preferred, splitMinX, splitMaxX);
  }();

  railPos.x =
      (side == DockDrawerSide::Left) ? (hingeX - visibleRailWidth) : hingeX;
  railPos.x = std::clamp(railPos.x, splitMinX, splitMaxX - railSize.x);

  float railTopY = branchMinY;
  if (hasValidBarRect) {
    railTopY = ImMax(railTopY, tabBar->BarRect.Max.y - 1.0f);
  }
  railPos.y = railTopY;
  railSize.y = ImMax(1.0f, branchMaxY - railTopY);

  char railWindowName[64];
  std::snprintf(railWindowName, sizeof(railWindowName), "##DockRail_%c_%08X",
                side == DockDrawerSide::Left ? 'L' : 'R',
                target.splitParent->ID);

  ImGuiWindowFlags railFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoFocusOnAppearing;

  ImGui::SetNextWindowPos(railPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(railSize, ImGuiCond_Always);
  if (target.drawerBranch->HostWindow) {
    ImGui::SetNextWindowViewport(target.drawerBranch->HostWindow->ViewportId);
  }
  ImGui::SetNextWindowBgAlpha(0.94f * reveal);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  if (ImGui::Begin(railWindowName, nullptr, railFlags)) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, 1.0f));
    ImDrawList *draw = ImGui::GetWindowDrawList();

    const ImGuiStyle &style = ImGui::GetStyle();
    const float slotSpacing = ImMax(1.0f, style.ItemInnerSpacing.y);
    const ImVec2 railMin = ImGui::GetWindowPos();
    const ImVec2 railMax(railMin.x + ImGui::GetWindowSize().x,
                         railMin.y + ImGui::GetWindowSize().y);
    const ImRect railRect(railMin, railMax);
    draw->AddRectFilled(railRect.Min, railRect.Max,
                        ImGui::GetColorU32(ImGuiCol_Tab));
    draw->AddRect(railRect.Min, railRect.Max,
                  ImGui::GetColorU32(ImGuiCol_Border));

    const bool tabBarFocused =
        (tabBar->Flags & ImGuiTabBarFlags_IsFocused) != 0;
    const float minSlotHeight = ImGui::GetFrameHeight() * 0.86f;
    const float maxSlotHeight = ImGui::GetFrameHeight() * 2.55f;
    const float slotWidth = ImMax(3.0f, ImGui::GetContentRegionAvail().x);
    float cursorY = ImGui::GetCursorPosY() + style.FramePadding.y;

    for (int i = 0; i < tabBar->Tabs.Size; ++i) {
      ImGuiTabItem *tab = &tabBar->Tabs[i];
      const char *tabName = ImGui::TabBarGetTabName(tabBar, tab);
      const bool selected = (tabBar->SelectedTabId == tab->ID) ||
                            (tabBar->VisibleTabId == tab->ID);
      const ImVec2 labelSize = ImGui::CalcTextSize(tabName);
      const float slotHeight =
          std::clamp(labelSize.x + style.FramePadding.x * 2.0f, minSlotHeight,
                     maxSlotHeight);

      ImGui::PushID(static_cast<int>(tab->ID));
      ImGui::SetCursorPosY(cursorY);
      ImVec2 slotPos = ImGui::GetCursorScreenPos();
      ImVec2 slotSize(slotWidth, slotHeight);
      if (ImGui::InvisibleButton("##SideTab", slotSize)) {
        queueDrawerTabFocus(state, tabBar, tab->ID);
        state.collapsed = false;
      }
      const bool hovered = ImGui::IsItemHovered();
      if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
      const ImRect slotRect(
          slotPos, ImVec2(slotPos.x + slotSize.x, slotPos.y + slotSize.y));

      const ImU32 bg =
          selected
              ? ImGui::GetColorU32(tabBarFocused ? ImGuiCol_TabSelected
                                                 : ImGuiCol_TabDimmedSelected)
              : (hovered
                     ? ImGui::GetColorU32(ImGuiCol_TabHovered)
                     : ImGui::GetColorU32(tabBarFocused ? ImGuiCol_Tab
                                                        : ImGuiCol_TabDimmed));
      const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
      const ImU32 overline = ImGui::GetColorU32(
          tabBarFocused ? ImGuiCol_TabSelectedOverline
                        : ImGuiCol_TabDimmedSelectedOverline);

      ImDrawFlags roundFlags = (side == DockDrawerSide::Left)
                                   ? (ImDrawFlags_RoundCornersTopRight |
                                      ImDrawFlags_RoundCornersBottomRight)
                                   : (ImDrawFlags_RoundCornersTopLeft |
                                      ImDrawFlags_RoundCornersBottomLeft);
      draw->AddRectFilled(slotRect.Min, slotRect.Max, bg, style.TabRounding,
                          roundFlags);
      draw->AddRect(slotRect.Min, slotRect.Max, border, style.TabRounding,
                    roundFlags);
      if (selected) {
        const float overlineThickness = ImMax(1.0f, style.TabBarOverlineSize);
        if (side == DockDrawerSide::Left) {
          draw->AddRectFilled(
              ImVec2(slotRect.Max.x - overlineThickness, slotRect.Min.y + 1.0f),
              ImVec2(slotRect.Max.x, slotRect.Max.y - 1.0f), overline);
        } else {
          draw->AddRectFilled(
              ImVec2(slotRect.Min.x, slotRect.Min.y + 1.0f),
              ImVec2(slotRect.Min.x + overlineThickness, slotRect.Max.y - 1.0f),
              overline);
        }
      }

      drawCollapsedDrawerHandleGrip(draw, slotRect, side,
                                    ImGui::GetColorU32(ImGuiCol_TextDisabled));

      const ImU32 textCol = ImGui::GetColorU32(
          (selected || hovered) ? ImGuiCol_Text : ImGuiCol_TextDisabled);
      const float baseFontSize = ImGui::GetFontSize();
      const float minRailFont = baseFontSize * 0.64f;
      const float maxRailFont = baseFontSize * 0.92f;
      float railFontSize =
          std::clamp(baseFontSize * 0.82f, minRailFont, maxRailFont);
      const ImVec2 unscaledText =
          ImGui::GetFont()->CalcTextSizeA(railFontSize, FLT_MAX, 0.0f, tabName);
      if (unscaledText.x > 0.5f && unscaledText.y > 0.5f) {
        const float fitToWidth = (slotRect.GetWidth() - 8.0f) / unscaledText.y;
        const float fitToHeight =
            (slotRect.GetHeight() - 10.0f) / unscaledText.x;
        const float fitScale = ImMin(fitToWidth, fitToHeight);
        railFontSize =
            std::clamp(railFontSize * fitScale, minRailFont, maxRailFont);
      }
      addRotatedText90CW(draw, ImGui::GetFont(), railFontSize, slotRect,
                         textCol, tabName);
      if (hovered) {
        ImGui::SetItemTooltip("%s", tabName);
      }
      ImGui::PopID();
      cursorY += slotHeight + slotSpacing;
    }
    ImGui::PopStyleVar();
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}

void renderCollapsedBottomDockRail(DockDrawerState &state,
                                   const DockDrawerTarget &target,
                                   float railHeight, float revealAmount) {
  if (!target.drawerBranch || !target.splitParent)
    return;
  ImGuiTabBar *tabBar = target.drawerBranch->TabBar;
  if (!tabBar || tabBar->Tabs.Size <= 0)
    return;

  const float reveal = std::clamp(revealAmount, 0.0f, 1.0f);
  if (reveal <= 0.001f)
    return;

  const float splitMinY = target.splitParent->Pos.y;
  const float splitMaxY =
      target.splitParent->Pos.y + target.splitParent->Size.y;
  const float splitHeight = ImMax(1.0f, splitMaxY - splitMinY);
  const float fullRailHeight =
      std::clamp(ImMax(railHeight, 20.0f), 8.0f, splitHeight);
  const float visibleRailHeight = ImMax(1.0f, fullRailHeight * reveal);

  const float branchMinX = target.drawerBranch->Pos.x;
  const float branchMaxX =
      target.drawerBranch->Pos.x + target.drawerBranch->Size.x;
  const float branchMinY = target.drawerBranch->Pos.y;

  ImVec2 railPos(branchMinX, std::clamp(branchMinY, splitMinY,
                                        splitMaxY - visibleRailHeight));
  ImVec2 railSize(ImMax(1.0f, branchMaxX - branchMinX), visibleRailHeight);

  char railWindowName[64];
  std::snprintf(railWindowName, sizeof(railWindowName), "##DockRail_B_%08X",
                target.splitParent->ID);

  ImGuiWindowFlags railFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoFocusOnAppearing;

  ImGui::SetNextWindowPos(railPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(railSize, ImGuiCond_Always);
  if (target.drawerBranch->HostWindow) {
    ImGui::SetNextWindowViewport(target.drawerBranch->HostWindow->ViewportId);
  }
  ImGui::SetNextWindowBgAlpha(0.96f * reveal);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 2.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  if (ImGui::Begin(railWindowName, nullptr, railFlags)) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImGuiStyle &style = ImGui::GetStyle();
    const ImRect railRect(
        ImGui::GetWindowPos(),
        ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
               ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    draw->AddRectFilled(railRect.Min, railRect.Max,
                        ImGui::GetColorU32(ImGuiCol_Tab), style.WindowRounding,
                        ImDrawFlags_RoundCornersTop);
    draw->AddRect(railRect.Min, railRect.Max,
                  ImGui::GetColorU32(ImGuiCol_Border), style.WindowRounding,
                  ImDrawFlags_RoundCornersTop);

    const bool tabBarFocused =
        (tabBar->Flags & ImGuiTabBarFlags_IsFocused) != 0;
    const float baseFontSize = ImGui::GetFontSize();
    const float handleFontSize = std::clamp(
        baseFontSize * 0.84f, baseFontSize * 0.72f, baseFontSize * 0.90f);
    const float tabHeight = ImMax(16.0f, railRect.GetHeight() - 4.0f);
    float cursorX = ImGui::GetCursorPosX();

    for (int i = 0; i < tabBar->Tabs.Size; ++i) {
      ImGuiTabItem *tab = &tabBar->Tabs[i];
      const char *tabName = ImGui::TabBarGetTabName(tabBar, tab);
      const bool selected = (tabBar->SelectedTabId == tab->ID) ||
                            (tabBar->VisibleTabId == tab->ID);
      const ImVec2 labelSize = ImGui::GetFont()->CalcTextSizeA(
          handleFontSize, FLT_MAX, 0.0f, tabName);
      const float tabWidth = std::clamp(labelSize.x + 26.0f, 58.0f, 124.0f);

      ImGui::PushID(static_cast<int>(tab->ID));
      ImGui::SetCursorPos(ImVec2(cursorX, ImGui::GetCursorPosY()));
      ImVec2 slotPos = ImGui::GetCursorScreenPos();
      ImVec2 slotSize(tabWidth, tabHeight);
      if (ImGui::InvisibleButton("##BottomTab", slotSize)) {
        queueDrawerTabFocus(state, tabBar, tab->ID);
        state.collapsed = false;
      }
      const bool hovered = ImGui::IsItemHovered();
      if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }

      const ImRect slotRect(
          slotPos, ImVec2(slotPos.x + slotSize.x, slotPos.y + slotSize.y));
      const ImU32 bg =
          selected
              ? ImGui::GetColorU32(tabBarFocused ? ImGuiCol_TabSelected
                                                 : ImGuiCol_TabDimmedSelected)
              : (hovered
                     ? ImGui::GetColorU32(ImGuiCol_TabHovered)
                     : ImGui::GetColorU32(tabBarFocused ? ImGuiCol_Tab
                                                        : ImGuiCol_TabDimmed));
      draw->AddRectFilled(slotRect.Min, slotRect.Max, bg, style.FrameRounding,
                          ImDrawFlags_RoundCornersTop);
      draw->AddRect(slotRect.Min, slotRect.Max,
                    ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding,
                    ImDrawFlags_RoundCornersTop);
      if (selected) {
        const float accentHeight = ImMax(1.0f, style.TabBarOverlineSize + 1.0f);
        draw->AddRectFilled(
            ImVec2(slotRect.Min.x + 1.0f, slotRect.Max.y - accentHeight),
            ImVec2(slotRect.Max.x - 1.0f, slotRect.Max.y),
            ImGui::GetColorU32(tabBarFocused
                                   ? ImGuiCol_TabSelectedOverline
                                   : ImGuiCol_TabDimmedSelectedOverline),
            style.FrameRounding, ImDrawFlags_RoundCornersTop);
      }

      drawCollapsedDrawerHandleGrip(draw, slotRect, DockDrawerSide::Bottom,
                                    ImGui::GetColorU32(ImGuiCol_TextDisabled));

      const ImU32 textCol = ImGui::GetColorU32(
          (selected || hovered) ? ImGuiCol_Text : ImGuiCol_TextDisabled);
      const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(
          handleFontSize, FLT_MAX, 0.0f, tabName);
      const ImVec2 textPos(
          slotRect.Min.x + (slotRect.GetWidth() - textSize.x) * 0.5f,
          slotRect.Min.y + 9.0f +
              (slotRect.GetHeight() - 9.0f - textSize.y) * 0.5f);
      draw->AddText(ImGui::GetFont(), handleFontSize, textPos, textCol,
                    tabName);

      if (hovered) {
        ImGui::SetItemTooltip("%s", tabName);
      }
      ImGui::PopID();
      cursorX += tabWidth + style.ItemSpacing.x;
    }

    ImGui::PopStyleVar();
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}

DockDrawerTarget findDockDrawerTarget(const char *const *anchorWindows,
                                      int anchorCount, DockDrawerSide side) {
  const ImGuiAxis axis =
      (side == DockDrawerSide::Bottom) ? ImGuiAxis_Y : ImGuiAxis_X;

  for (int anchorIdx = 0; anchorIdx < anchorCount; ++anchorIdx) {
    const char *name = anchorWindows[anchorIdx];
    ImGuiWindow *anchor = findWindowByVisibleTitle(name);
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

        if (matchesSide) {
          return DockDrawerTarget{parent, drawerCandidate, oppositeCandidate};
        }
      }
      current = parent;
    }
  }

  return {};
}

void updateDockDrawerAnimation(DockDrawerState &state,
                               const DockDrawerTarget &target,
                               DockDrawerSide side,
                               const char *const *anchorWindows,
                               int anchorCount) {
  if (!target.splitParent || !target.drawerBranch || !target.oppositeBranch ||
      !target.splitParent->ChildNodes[0] ||
      !target.splitParent->ChildNodes[1]) {
    state.activeSplitParentId = 0;
    state.collapsed = false;
    state.openAmount = 1.0f;
    state.expandedExtent = 0.0f;
    state.pendingTabFocusId = 0;
    return;
  }

  if (state.activeSplitParentId != target.splitParent->ID) {
    state.activeSplitParentId = target.splitParent->ID;
    state.collapsed = false;
    state.openAmount = 1.0f;
    state.expandedExtent = (side == DockDrawerSide::Bottom)
                               ? ImMax(0.0f, target.drawerBranch->Size.y)
                               : ImMax(0.0f, target.drawerBranch->Size.x);
    state.pendingTabFocusId = 0;
  }

  ImGuiTabBar *tabBar = target.drawerBranch->TabBar;
  applyPendingDrawerTabFocus(state, tabBar);
  constexpr float kCollapsedSideRailWidth = 20.0f;
  constexpr float kCollapsedBottomRailHeight = 24.0f;
  float collapsedExtent = (side == DockDrawerSide::Bottom)
                              ? kCollapsedBottomRailHeight
                              : kCollapsedSideRailWidth;
  if (tabBar) {
    if (side == DockDrawerSide::Bottom) {
      collapsedExtent = ImMax(collapsedExtent, kCollapsedBottomRailHeight);
    } else {
      collapsedExtent = ImMax(collapsedExtent, kCollapsedSideRailWidth);
    }
  }

  DockTabInteractionState interaction =
      queryDockTabInteraction(target, anchorWindows, anchorCount);
  if (interaction.doubleClicked) {
    if (!state.collapsed) {
      const float liveExtent = (side == DockDrawerSide::Bottom)
                                   ? target.drawerBranch->Size.y
                                   : target.drawerBranch->Size.x;
      if (liveExtent > 1.0f) {
        state.expandedExtent = liveExtent;
      }
    }
    state.collapsed = !state.collapsed;
  } else if (state.collapsed && interaction.clicked) {
    state.collapsed = false;
  }

  const float totalExtent = (side == DockDrawerSide::Bottom)
                                ? ImMax(0.0f, target.splitParent->Size.y)
                                : ImMax(0.0f, target.splitParent->Size.x);
  const float minOppositeExtent =
      (side == DockDrawerSide::Bottom) ? 96.0f : 220.0f;
  const float maxDrawerExtent =
      ImMax(collapsedExtent, totalExtent - minOppositeExtent);
  const float expandedMinExtent = ImMin(
      collapsedExtent + ((side == DockDrawerSide::Bottom) ? 24.0f : 40.0f),
      maxDrawerExtent);

  const bool captureExpandedExtent =
      !state.collapsed && state.openAmount >= 0.995f;
  if (captureExpandedExtent) {
    const float liveExtent = (side == DockDrawerSide::Bottom)
                                 ? target.drawerBranch->Size.y
                                 : target.drawerBranch->Size.x;
    const float clampedLiveExtent =
        std::clamp(liveExtent, collapsedExtent, maxDrawerExtent);
    if (clampedLiveExtent > collapsedExtent + 1.0f) {
      state.expandedExtent = clampedLiveExtent;
    }
  }

  if (state.expandedExtent < collapsedExtent + 1.0f) {
    const float defaultRatio = (side == DockDrawerSide::Bottom) ? 0.30f : 0.22f;
    state.expandedExtent = std::clamp(totalExtent * defaultRatio,
                                      expandedMinExtent, maxDrawerExtent);
  } else {
    state.expandedExtent =
        std::clamp(state.expandedExtent, expandedMinExtent, maxDrawerExtent);
  }

  const float targetOpen = state.collapsed ? 0.0f : 1.0f;
  const float blend = 1.0f - std::exp(-12.0f * ImGui::GetIO().DeltaTime);
  state.openAmount += (targetOpen - state.openAmount) * blend;
  if (std::fabs(state.openAmount - targetOpen) < 0.001f) {
    state.openAmount = targetOpen;
  }
  state.openAmount = std::clamp(state.openAmount, 0.0f, 1.0f);

  const float desiredDrawerExtent =
      collapsedExtent +
      (state.expandedExtent - collapsedExtent) * state.openAmount;
  const float drawerExtent =
      std::clamp(desiredDrawerExtent, collapsedExtent, maxDrawerExtent);
  const float oppositeExtent =
      ImMax(minOppositeExtent, totalExtent - drawerExtent);

  target.splitParent->AuthorityForSize = ImGuiDataAuthority_DockNode;
  target.drawerBranch->AuthorityForSize = ImGuiDataAuthority_DockNode;
  target.oppositeBranch->AuthorityForSize = ImGuiDataAuthority_DockNode;
  if (side == DockDrawerSide::Bottom) {
    target.drawerBranch->Size.y = drawerExtent;
    target.drawerBranch->SizeRef.y = drawerExtent;
    target.oppositeBranch->Size.y = oppositeExtent;
    target.oppositeBranch->SizeRef.y = oppositeExtent;
  } else {
    target.drawerBranch->Size.x = drawerExtent;
    target.drawerBranch->SizeRef.x = drawerExtent;
    target.oppositeBranch->Size.x = oppositeExtent;
    target.oppositeBranch->SizeRef.x = oppositeExtent;
  }

  const float railReveal = std::clamp(1.0f - state.openAmount, 0.0f, 1.0f);
  const bool useCollapsedRail = railReveal > 0.01f;
  ImGuiDockNodeFlags desiredFlags = target.drawerBranch->LocalFlags;
  if (useCollapsedRail) {
    desiredFlags |= ImGuiDockNodeFlags_HiddenTabBar;
  } else {
    desiredFlags &= ~ImGuiDockNodeFlags_HiddenTabBar;
  }
  if (desiredFlags != target.drawerBranch->LocalFlags) {
    target.drawerBranch->SetLocalFlags(desiredFlags);
  }

  if (useCollapsedRail) {
    if (side == DockDrawerSide::Bottom) {
      renderCollapsedBottomDockRail(state, target, collapsedExtent, railReveal);
    } else {
      renderCollapsedSideDockRail(state, target, side, collapsedExtent,
                                  railReveal);
    }
  }
}

void updateDockDrawerAnimations() {
  static int lastUpdatedFrame = -1;
  const int currentFrame = ImGui::GetFrameCount();
  if (lastUpdatedFrame == currentFrame) {
    return;
  }
  lastUpdatedFrame = currentFrame;

  static DockDrawerState leftState;
  static DockDrawerState rightState;
  static DockDrawerState bottomState;

  static const char *kLeftAnchors[] = {"Hierarchy", "Camera"};
  static const char *kRightAnchors[] = {"Inspector", "Environment"};
  static const char *kBottomAnchors[] = {"Project", "Project Settings",
                                         "Animation", "AI Pathfinding"};

  updateDockDrawerAnimation(
      leftState,
      findDockDrawerTarget(kLeftAnchors, IM_ARRAYSIZE(kLeftAnchors),
                           DockDrawerSide::Left),
      DockDrawerSide::Left, kLeftAnchors, IM_ARRAYSIZE(kLeftAnchors));
  updateDockDrawerAnimation(
      rightState,
      findDockDrawerTarget(kRightAnchors, IM_ARRAYSIZE(kRightAnchors),
                           DockDrawerSide::Right),
      DockDrawerSide::Right, kRightAnchors, IM_ARRAYSIZE(kRightAnchors));
  updateDockDrawerAnimation(
      bottomState,
      findDockDrawerTarget(kBottomAnchors, IM_ARRAYSIZE(kBottomAnchors),
                           DockDrawerSide::Bottom),
      DockDrawerSide::Bottom, kBottomAnchors, IM_ARRAYSIZE(kBottomAnchors));
}
} // namespace

void Engine::updateDockDrawerInteractions() {
  if (pendingWorkspaceReload || workspaceLayoutDirty ||
      glfwGetTime() < workspaceLayoutStabilizeUntil) {
    return;
  }
  updateDockDrawerAnimations();
}


