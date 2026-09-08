#pragma once
#include "Engine.h"

namespace GizmoToolbar {

enum class Icon {
  Translate,
  Rotate,
  Scale,
  Bounds,
  Universal,
  Mesh,
  GizmoToggle,
  GridToggle,
  SnapToggle,
  LocalMode,
  WorldMode,
  UiWorldToggle
};

ImVec4 ScaleColor(const ImVec4 &c, float s);

bool TextButton(const char *label, bool active, const ImVec2 &size,
                ImU32 base, ImU32 hover, ImU32 activeCol, ImU32 accent,
                ImU32 textColor);

bool IconButton(const char *id, Icon icon, bool active,
                const ImVec2 &size, ImU32 baseColor, ImU32 hoverColor,
                ImU32 activeColor, ImU32 accentColor, ImU32 iconColor);

bool TextButton(const char *id, const char *label, bool active,
                const ImVec2 &size, ImU32 baseColor, ImU32 hoverColor,
                ImU32 activeColor, ImU32 borderColor, ImVec4 textColor);

bool ModeButton(const char *label, bool active, const ImVec2 &size,
                ImVec4 baseColor, ImVec4 activeColor, ImVec4 textColor);

} // namespace GizmoToolbar
