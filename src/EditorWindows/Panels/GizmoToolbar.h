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
  UiWorldToggle,
  FreeformMode,
  AssemblageMode
};

// An authored toolbar PNG. The editor resolves these through the renderer's texture cache with
// point filtering and hands them to IconButton; an empty id falls back to the vector drawing
// for that Icon, so the toolbar still works if the art is missing.
struct IconTexture {
  ImTextureID id = static_cast<ImTextureID>(0);
  ImVec2 nativeSize = ImVec2(0.0f, 0.0f);  // Source pixel size. 0 = unknown: fill the button.
  bool flipY = true;                       // GL textures arrive bottom-up.
};

// Engine-root-relative path to an icon's authored art, or nullptr when it has none.
const char *IconTexturePath(Icon icon);

ImVec4 ScaleColor(const ImVec4 &c, float s);

bool TextButton(const char *label, bool active, const ImVec2 &size,
                ImU32 base, ImU32 hover, ImU32 activeCol, ImU32 accent,
                ImU32 textColor);

// Toggle button. `active` is what makes the icon read as on: an inactive icon is drawn
// darker, never a different shape. Authored art is drawn 1:1 on whole pixels.
bool IconButton(const char *id, Icon icon, bool active,
                const ImVec2 &size, ImU32 baseColor, ImU32 hoverColor,
                ImU32 activeColor, ImU32 accentColor, ImU32 iconColor,
                const IconTexture &texture = IconTexture{});

bool TextButton(const char *id, const char *label, bool active,
                const ImVec2 &size, ImU32 baseColor, ImU32 hoverColor,
                ImU32 activeColor, ImU32 borderColor, ImVec4 textColor);

bool ModeButton(const char *label, bool active, const ImVec2 &size,
                ImVec4 baseColor, ImVec4 activeColor, ImVec4 textColor);

} // namespace GizmoToolbar
