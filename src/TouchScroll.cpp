#include "Engine.h"

#ifdef __ANDROID__

#include "ThirdParty/ModuGUI/imgui.h"
#include "ThirdParty/ModuGUI/imgui_internal.h"

#include <cmath>

bool Engine::updateAndroidTouchScroll(float px, float py, bool active) {
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiIO &io = g.IO;

    constexpr double kLongPressSeconds = 0.30;

    static bool tracking = false;
    static bool decided = false;
    static bool isScroll = false;
    static ImVec2 startPos(0.0f, 0.0f);
    static ImVec2 lastPos(0.0f, 0.0f);
    static ImGuiWindow *target = nullptr;
    static double holdStart = 0.0;
    static int longPressPhase = 0;
    static bool longPressDead = false;

    if (!active) {
        if (longPressPhase == 1) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        }
        tracking = false;
        decided = false;
        isScroll = false;
        target = nullptr;
        longPressPhase = 0;
        longPressDead = false;
        return false;
    }

    const float dpi = (uiDpiScale > 0.0f) ? uiDpiScale : 1.0f;
    const float slop = 16.0f * dpi;

    if (!tracking) {
        tracking = true;
        decided = false;
        isScroll = false;
        startPos = lastPos = ImVec2(px, py);
        target = g.HoveredWindow;
        holdStart = g.Time;
        longPressPhase = 0;
        longPressDead = false;
        return true;
    }

    const ImVec2 delta(px - lastPos.x, py - lastPos.y);
    lastPos = ImVec2(px, py);

    if (!decided && g.HoveredWindow != nullptr) {
        target = g.HoveredWindow;
    }

    if (longPressPhase == 1) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        longPressPhase = 2;
        return false;
    }
    if (longPressPhase == 2) {
        return false;
    }
    if (!longPressDead) {
        const float ex = px - startPos.x;
        const float ey = py - startPos.y;
        if (ex * ex + ey * ey >= slop * slop) {
            longPressDead = true;
        } else if (!io.WantTextInput && g.HoveredWindow != nullptr &&
                   (g.Time - holdStart) >= kLongPressSeconds) {
            if (g.ActiveId != 0) {
                ImGui::ClearActiveID();
            }
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
            longPressPhase = 1;
            return false;
        }
    }

    if (!decided) {
        const float ex = px - startPos.x;
        const float ey = py - startPos.y;
        if (ex * ex + ey * ey >= slop * slop) {
            decided = true;
            ImGuiWindow *w = target;
            const bool canScroll =
                w != nullptr && (w->ScrollMax.y > 0.0f ||
                                 ((w->Flags & ImGuiWindowFlags_HorizontalScrollbar) &&
                                  w->ScrollMax.x > 0.0f));
            isScroll = canScroll && !io.WantTextInput;
        }
    }

    if (decided && isScroll && target != nullptr) {
        if (g.ActiveId != 0) {
            ImGui::ClearActiveID();
        }
        if (target->ScrollMax.y > 0.0f) {
            ImGui::SetScrollY(target, target->Scroll.y - delta.y);
        }
        if ((target->Flags & ImGuiWindowFlags_HorizontalScrollbar) &&
            target->ScrollMax.x > 0.0f) {
            ImGui::SetScrollX(target, target->Scroll.x - delta.x);
        }
        return false;
    }

    return !decided || !isScroll;
}

#else

bool Engine::updateAndroidTouchScroll(float, float, bool) { return true; }

#endif // __ANDROID__
