#include "EditorWindows/VREditorMouseInput.h"
#include <cstdio>

static int failures = 0;
static int checks = 0;
static void check(bool pass, const char* label) {
    ++checks;
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
    if (!pass) ++failures;
}

static void drawWindows(bool children = false) {
    for (const char* name : {"A", "B", "NoMouse"}) {
        ImGui::SetNextWindowPos({20, 20});
        ImGui::SetNextWindowSize({260, 240});
        const bool noMouse = name[0] == 'N';
        ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings | (noMouse ? ImGuiWindowFlags_NoMouseInputs : 0));
        if (children && name[0] == 'A') {
            ImGui::BeginChild("Child", {210, 180});
            ImGui::TextUnformatted("Child content");
            ImGui::EndChild();
        } else ImGui::TextUnformatted(name);
        ImGui::End();
    }
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = {800, 600};
    io.DeltaTime = 1.f / 60;
    io.IniFilename = nullptr;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (int i = 0; i < 2; ++i) {
        ImGui::NewFrame(); drawWindows(); ImGui::Render();
    }
    auto* a = ImGui::FindWindowByName("A");
    auto* b = ImGui::FindWindowByName("B");
    auto* noMouse = ImGui::FindWindowByName("NoMouse");
    io.AddMousePosEvent(80, 80);
    VREditorMouseInput::NewFrame(0);
    check(ImGui::GetCurrentContext()->HoveredWindow == b, "unrestricted pointer follows normal overlapping window order");
    drawWindows(); ImGui::Render();
    VREditorMouseInput::NewFrame(a->ID);
    check(ImGui::GetCurrentContext()->HoveredWindow == a, "controller hit selects a surface behind an overlapping virtual window");
    check(!(b->Flags & ImGuiWindowFlags_NoMouseInputs), "other panels regain mouse flags after hover selection");
    check((noMouse->Flags & ImGuiWindowFlags_NoMouseInputs) != 0, "permanent NoMouseInputs flags are preserved");
    drawWindows(); ImGui::Render();
    VREditorMouseInput::NewFrame(b->ID);
    check(ImGui::GetCurrentContext()->HoveredWindow == b, "pointer can switch surfaces on the next frame");
    drawWindows(); ImGui::Render();
    VREditorMouseInput::NewFrame(1234567);
    check(ImGui::GetCurrentContext()->HoveredWindow == nullptr, "missing surfaces cannot forward clicks to unrelated panels");
    check(!(a->Flags & ImGuiWindowFlags_NoMouseInputs) && !(b->Flags & ImGuiWindowFlags_NoMouseInputs), "a missing surface restores all temporary input flags");
    drawWindows(); ImGui::Render();
    VREditorMouseInput::NewFrame(0);
    check(ImGui::GetCurrentContext()->HoveredWindow == b, "clearing the hit root restores ordinary mouse routing");
    drawWindows(true); ImGui::Render();
    VREditorMouseInput::NewFrame(a->ID);
    auto* hovered = ImGui::GetCurrentContext()->HoveredWindow;
    check(hovered && hovered != a && hovered->RootWindow == a, "child windows on the selected surface remain interactive");
    drawWindows(true); ImGui::Render();
    ImGui::DestroyContext();
    std::printf("VR mouse input: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
