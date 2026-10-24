#include "UILayout.h"
#include "UIControls.h"
#include "UIInspector.h"
#include "UIControlsData.h"
#include "ThirdParty/ModuGUI/imgui_internal.h"
#include <cassert>
#include <iostream>

static SceneObject Node(int id, int parent, glm::vec2 size) {
    SceneObject obj("Test", ObjectType::UIImage, id); obj.id = id; obj.parentId = parent; obj.hasUI = true;
    obj.ui.type = UIElementType::Panel; obj.ui.size = size; return obj;
}
int main() {
    static_assert(static_cast<int>(UIElementType::Canvas) == 1 && static_cast<int>(UIElementType::Sprite2D) == 6, "legacy IDs changed");
    std::vector<std::string> labels = {"", "A\nB", "x:y=\\z", "\xE2\x98\x83"}, decoded;
    UIControlsData::Split(UIControlsData::Join(labels), decoded); assert(labels == decoded);
    UIControlsData::Split("999:bad", decoded); assert(labels == decoded);
    std::vector<int> parents = {-1, 0, 1, -1}, decodedParents;
    UIControlsData::Split(UIControlsData::Join(parents), decodedParents); assert(parents == decodedParents);
    std::vector<SceneObject> scene;
    scene.push_back(Node(0, -1, {400, 200})); scene[0].ui.layout = UILayoutMode::Horizontal; scene[0].ui.padding = glm::vec4(10);
    scene.push_back(Node(1, 0, {100, 40})); scene.push_back(Node(2, 0, {100, 40})); scene[2].ui.flexibleSize = 1;
    UILayout::Context layout(scene);
    auto p = layout.Resolve(scene[1], {400, 200}, {100, 40}); assert(p.position == glm::vec2(10));
    p = layout.Resolve(scene[2], {400, 200}, {100, 40}); assert(p.position.x == 118 && p.size.x == 272);
    assert(scene[2].ui.size.x == 100 && scene[2].ui.position.x == 0);
    scene[0].ui.fitWidth = scene[0].ui.fitHeight = true;
    UILayout::Context fitter(scene); assert(fitter.Measure(scene[0]) == glm::vec2(228, 60));
    scene[0].ui.layout = UILayoutMode::None; scene[1].ui.responsive = true; scene[1].ui.size = glm::vec2(-20, -20);
    UILayout::Context responsive(scene); p = responsive.Resolve(scene[1], {400, 200}, {100, 40}); assert(p.size == glm::vec2(380, 180));
    assert(!ClipsUIChildren(scene[0].ui)); scene[0].ui.clipChildren = true; assert(ClipsUIChildren(scene[0].ui));
    scene[0].ui.type = UIElementType::Foldout; scene[0].ui.open = false;
    assert(!UIControls::ContentVisible(scene[1], scene));
    // Split panes fill the split view on either side of the handle.
    scene[0].ui.type = UIElementType::SplitView; scene[0].ui.vertical = false; scene[0].ui.sliderValue = .25f; scene[0].ui.size = {406, 200};
    UILayout::Context split(scene);
    p = split.Resolve(scene[1], {406, 200}, {100, 40}); assert(p.position == glm::vec2(0) && p.size == glm::vec2(100, 200));
    p = split.Resolve(scene[2], {406, 200}, {100, 40}); assert(p.position == glm::vec2(106, 0) && p.size == glm::vec2(300, 200));
    // Creation defaults give every control starter content and a readable palette, and old
    // scenes saved with a dark tint still get fills that stand out from their track.
    for (int type = static_cast<int>(UIElementType::Panel); type <= static_cast<int>(UIElementType::DockContainer); ++type) {
        UIElementComponent fresh; UIControls::ApplyCreationDefaults(fresh, static_cast<UIElementType>(type));
        assert(fresh.type == static_cast<UIElementType>(type) && fresh.size.x > 0 && fresh.size.y > 0);
        if (UIControls::UsesItems(fresh.type)) assert(!fresh.items.empty());
    }
    UIElementComponent slate; slate.color = glm::vec4(.18f, .2f, .25f, 1);
    const auto pal = UIControls::ResolvePalette(slate);
    assert(std::abs(UIControls::Luminance(pal.accent) - UIControls::Luminance(pal.background)) >= .22f);
    // Moving an item keeps tree parents, expansion and selection pointing at the same items.
    UIElementComponent tree; tree.type = UIElementType::TreeView;
    tree.items = {"A", "B", "C"}; tree.itemParents = {-1, 0, 1}; tree.expandedItems = {0, 1}; tree.selectedIndex = 2;
    UIControls::MoveItem(tree, 0, 2); // B, C, A
    assert((tree.items == std::vector<std::string>{"B", "C", "A"}));
    assert((tree.itemParents == std::vector<int>{2, 0, -1}) && tree.selectedIndex == 1);
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(1024, 768); io.DeltaTime = 1.f / 60;
    io.IniFilename = nullptr; io.Fonts->Build(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    // Draw every control for several frames: validates balanced stacks and text buffer resizing.
    for (int type = static_cast<int>(UIElementType::Panel); type <= static_cast<int>(UIElementType::DockContainer); ++type) {
        scene[0].ui.type = static_cast<UIElementType>(type); scene[0].ui.items = {"Root", "Child", "Other"};
        scene[0].ui.itemParents = {-1, 0, -1}; scene[0].ui.open = false;
        for (int frame = 0; frame < 3; ++frame) {
            ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1024, 768));
            ImGui::Begin("test"); ImGui::PushID(type);
            assert(UIControls::Draw(scene[0], scene, ImVec2(20, 40), ImVec2(400, 400)));
            ImGui::PopID(); ImGui::End(); ImGui::Render(); assert(ImGui::GetCurrentContext()->ErrorCountCurrentFrame == 0);
        }
    }
    scene[0].ui.type = UIElementType::DockContainer;
    scene[1].ui.type = UIElementType::Canvas; scene[1].ui.open = true;
    assert(UIControls::IsDockedCanvas(scene[1], scene));
    int dockCallbacks = 0;
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1024, 768));
        ImGui::Begin("dock test"); ImGui::PushID("dock");
        UIControls::Draw(scene[0], scene, ImVec2(20, 40), ImVec2(700, 500), true, 0,
            [&](SceneObject& panel, ImVec2, ImVec2) { assert(panel.id == 1); ++dockCallbacks; ImGui::TextUnformatted("Canvas content"); });
        ImGui::PopID(); ImGui::End(); ImGui::Render();
        assert(ImGui::GetCurrentContext()->ErrorCountCurrentFrame == 0);
    }
    assert(dockCallbacks > 0);
    auto drawFrame = [&](bool clip = false, bool live = true) {
        ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1024, 768));
        ImGui::SetNextWindowFocus(); ImGui::Begin("interaction"); ImGui::PushID("control");
        if (clip) ImGui::PushClipRect(ImVec2(20, 40), ImVec2(35, 55), true);
        UIControls::Draw(scene[0], scene, ImVec2(20, 40), ImVec2(400, 300), live);
        if (clip) ImGui::PopClipRect();
        ImGui::PopID(); ImGui::End(); ImGui::Render();
        assert(ImGui::GetCurrentContext()->ErrorCountCurrentFrame == 0);
    };
    auto& control = scene[0].ui;
    control.type = UIElementType::Toggle; control.label = "Toggle"; control.checked = false; control.interactable = true;
    // The toggle is vertically centered in its 260px rect, so click the middle row.
    drawFrame(); io.AddMousePosEvent(30, 170); drawFrame(); io.AddMouseButtonEvent(0, true); drawFrame();
    io.AddMouseButtonEvent(0, false); drawFrame(); assert(control.checked && control.buttonPressed);
    control.checked = false; control.interactable = false;
    io.AddMouseButtonEvent(0, true); drawFrame(); io.AddMouseButtonEvent(0, false); drawFrame(); assert(!control.checked);
    control.interactable = true;
    io.AddMouseButtonEvent(0, true); drawFrame(true); io.AddMouseButtonEvent(0, false); drawFrame(true); assert(!control.checked);
    // Edit-time previews (Scene view, stopped Game view) ignore clicks without looking disabled.
    io.AddMouseButtonEvent(0, true); drawFrame(false, false); io.AddMouseButtonEvent(0, false); drawFrame(false, false); assert(!control.checked);
    control.type = UIElementType::InputField; control.inputText.clear(); control.requestFocus = true;
    drawFrame(); drawFrame(); io.AddInputCharactersUTF8("hello"); drawFrame();
    assert(control.inputText == "hello");
    io.AddKeyEvent(ImGuiKey_Enter, true); drawFrame(); assert(control.inputSubmitRevision > 0);
    io.AddKeyEvent(ImGuiKey_Enter, false); drawFrame();
    control.type = UIElementType::RangeSlider; control.sliderMin = 0; control.sliderMax = 10;
    control.sliderValue = 8; control.rangeUpper = 2; drawFrame(); assert(control.rangeUpper >= control.sliderValue);
    // Focused range slider: arrows move the lower handle, Shift+arrows the upper one.
    control.sliderValue = 2; control.rangeUpper = 8; control.requestFocus = true;
    drawFrame(); drawFrame();
    io.AddKeyEvent(ImGuiKey_RightArrow, true); drawFrame(); io.AddKeyEvent(ImGuiKey_RightArrow, false); drawFrame();
    assert(control.sliderValue > 2 && control.rangeUpper == 8);
    io.AddKeyEvent(ImGuiMod_Shift, true); io.AddKeyEvent(ImGuiKey_LeftArrow, true); drawFrame();
    io.AddKeyEvent(ImGuiKey_LeftArrow, false); io.AddKeyEvent(ImGuiMod_Shift, false); drawFrame();
    assert(control.rangeUpper < 8 && control.rangeUpper >= control.sliderValue);
    control.type = UIElementType::ListView; control.items.assign(100000, "Virtual row");
    control.virtualize = true; control.filterText.clear(); drawFrame();
    assert(ImGui::GetDrawData()->TotalVtxCount < 10000);
    assert(control.visibleItemIndices.empty()); // no O(N) filter scan for unfiltered virtualization
    // Offscreen contexts receive keyboard/gamepad input once and release it on focus loss.
    ImGuiContext* mainContext = ImGui::GetCurrentContext();
    io.KeysData[ImGuiKey_A - ImGuiKey_NamedKey_BEGIN].Down = true;
    io.KeysData[ImGuiKey_A - ImGuiKey_NamedKey_BEGIN].AnalogValue = 1;
    io.InputQueueCharacters.push_back('x');
    ImGuiContext* offscreen = ImGui::CreateContext(io.Fonts);
    auto& offscreenIO = ImGui::GetIO(); offscreenIO.DisplaySize = ImVec2(1024, 768); offscreenIO.DeltaTime = 1.f / 60; offscreenIO.IniFilename = nullptr;
    UIControls::ForwardKeyboardInput(offscreenIO, io, true);
    ImGui::NewFrame(); assert(ImGui::IsKeyDown(ImGuiKey_A)); assert(offscreenIO.InputQueueCharacters.Size > 0); ImGui::EndFrame();
    UIControls::ForwardKeyboardInput(offscreenIO, io, false);
    ImGui::NewFrame(); assert(!ImGui::IsKeyDown(ImGuiKey_A)); ImGui::EndFrame();
    ImGui::DestroyContext(offscreen); ImGui::SetCurrentContext(mainContext);
    ImGui::DestroyContext(); std::cout << "UI regression checks passed\n";
}

