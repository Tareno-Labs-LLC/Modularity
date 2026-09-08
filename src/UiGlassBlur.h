#pragma once

#include "imgui.h"

namespace Modularity {
namespace UiGlassBlur {

void Install();

void Shutdown();

bool EmitBackdropCapture(ImDrawList* drawList);

ImTextureID BackdropTexture();

} // namespace UiGlassBlur
} // namespace Modularity
