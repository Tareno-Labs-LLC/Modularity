#pragma once
#include "ThirdParty/ModuGUI/imgui_internal.h"
#include <vector>

namespace VREditorMouseInput {

// Panels can overlap in the virtual desktop while being separate surfaces in
// VR. Limit NewFrame's hover search to the surface hit by the controller ray.
// Use the pinned ImGui API instead of requiring a custom submodule function.
inline void NewFrame(ImGuiID hitTestRoot) {
    ImGuiContext& context = *ImGui::GetCurrentContext();
    static thread_local std::vector<ImGuiWindow*> blocked;
    blocked.clear();
    if (hitTestRoot) {
        blocked.reserve(context.Windows.Size);
        for (ImGuiWindow* window : context.Windows) {
            if (window->ID == hitTestRoot ||
                (window->RootWindow && window->RootWindow->ID == hitTestRoot) ||
                (window->RootWindowDockTree && window->RootWindowDockTree->ID == hitTestRoot) ||
                (window->Flags & ImGuiWindowFlags_NoMouseInputs)) continue;
            blocked.push_back(window);
            window->Flags |= ImGuiWindowFlags_NoMouseInputs;
        }
    }
    struct RestoreMouseInputs {
        std::vector<ImGuiWindow*>& windows;
        ~RestoreMouseInputs() {
            for (ImGuiWindow* window : windows) window->Flags &= ~ImGuiWindowFlags_NoMouseInputs;
        }
    } restore{blocked};
    ImGui::NewFrame();
}

} // namespace VREditorMouseInput
