// Renders every extended UI control into tools-output PNG-ready PPM with the editor's
// theme, so visual changes can be reviewed without launching the editor (whose GL window
// cannot be captured from a script). Build/run through tools/Render-UIGallery.ps1.
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "UIControls.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <fstream>
#include <iostream>

// Mirrors applyModularityStyle()/applyModularityShadeTheme() in src/EditorUI.cpp; that file
// pulls in the editor, so the values are copied here. Keep in step when the theme changes.
static ImVec4 Col(int r, int g, int b, float a = 1) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
static void ApplyEditorTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(3, 3); s.FramePadding = ImVec2(4, 4); s.ItemSpacing = ImVec2(10, 5);
    s.ItemInnerSpacing = ImVec2(2, 2); s.CellPadding = ImVec2(4, 2); s.IndentSpacing = 11; s.GrabMinSize = 8;
    s.WindowBorderSize = 0; s.ChildBorderSize = 1; s.PopupBorderSize = 1; s.FrameBorderSize = 0;
    s.ScrollbarSize = 11; s.ScrollbarPadding = 1; s.ButtonTextAlign = ImVec2(.5f, .5f);
    s.WindowRounding = 9; s.ChildRounding = 7; s.FrameRounding = 6; s.PopupRounding = 9;
    s.GrabRounding = 6; s.TabRounding = 7; s.ScrollbarRounding = 7; s.TabBorderSize = 0;
    s.CheckboxSwitch = true; s.SliderPill = false;
    const ImVec4 accent(.580f, .451f, .902f, 1), bright(.678f, .565f, .965f, 1), deep(.408f, .310f, .678f, 1);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = Col(228, 225, 236); c[ImGuiCol_TextDisabled] = Col(124, 118, 142);
    c[ImGuiCol_WindowBg] = Col(38, 33, 52); c[ImGuiCol_ChildBg] = Col(30, 26, 41); c[ImGuiCol_PopupBg] = Col(43, 37, 58);
    c[ImGuiCol_Border] = ImVec4(.271f, .239f, .376f, .55f); c[ImGuiCol_MenuBarBg] = Col(45, 39, 60);
    c[ImGuiCol_FrameBg] = Col(24, 21, 33); c[ImGuiCol_FrameBgHovered] = Col(31, 27, 43); c[ImGuiCol_FrameBgActive] = Col(36, 31, 49);
    c[ImGuiCol_Button] = Col(52, 45, 70); c[ImGuiCol_ButtonHovered] = Col(64, 55, 87); c[ImGuiCol_ButtonActive] = Col(41, 35, 56);
    c[ImGuiCol_Header] = ImVec4(.325f, .259f, .522f, 1); c[ImGuiCol_HeaderHovered] = Col(56, 48, 76); c[ImGuiCol_HeaderActive] = deep;
    c[ImGuiCol_Separator] = ImVec4(.086f, .075f, .122f, 1); c[ImGuiCol_ScrollbarBg] = Col(23, 20, 32);
    c[ImGuiCol_ScrollbarGrab] = Col(58, 51, 78); c[ImGuiCol_ScrollbarGrabHovered] = Col(72, 63, 96); c[ImGuiCol_ScrollbarGrabActive] = Col(87, 75, 118);
    c[ImGuiCol_CheckMark] = bright; c[ImGuiCol_SliderGrab] = Col(126, 108, 176); c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Tab] = Col(31, 27, 42); c[ImGuiCol_TabHovered] = Col(55, 47, 75); c[ImGuiCol_TabSelected] = Col(48, 42, 66);
    c[ImGuiCol_TabSelectedOverline] = accent; c[ImGuiCol_PlotHistogram] = accent; c[ImGuiCol_TableHeaderBg] = Col(45, 39, 60);
    c[ImGuiCol_TableBorderStrong] = Col(20, 17, 28); c[ImGuiCol_TableBorderLight] = Col(30, 26, 40);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, .022f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, .32f); c[ImGuiCol_NavCursor] = bright;
    ImGuiShadeTheme shade; ImGui::StyleShadeThemeDesktop(&shade);
    shade.Params[ImGuiShadeClass_Button][ImGuiShadeState_Normal].ColTopHighlight = IM_COL32(198, 178, 255, 34);
    shade.Params[ImGuiShadeClass_Grab][ImGuiShadeState_Normal].ColTopHighlight = IM_COL32(220, 206, 255, 52);
    shade.Params[ImGuiShadeClass_Header][ImGuiShadeState_Selected].Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_NoBevel;
    ImGui::SetShadeTheme(shade);
}

int main(int argc, char** argv) {
    const char* outPath = argc > 1 ? argv[1] : "build/Dev-Resources/ui-gallery.ppm";
    const int W = 1600, H = 1500; // offscreen, so not limited by the desktop size
    if (!glfwInit()) return 2;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    GLFWwindow* window = glfwCreateWindow(320, 240, "UI gallery", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 3; }
    glfwMakeContextCurrent(window); if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 4;
    GLuint fbo = 0, color = 0;
    glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 5;
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (ImFont* font = io.Fonts->AddFontFromFileTTF("Resources/Fonts/TheSunset.ttf", 15.5f)) io.FontDefault = font;
    else if (ImFont* fallback = io.Fonts->AddFontFromFileTTF("TheSunset.ttf", 15.5f)) io.FontDefault = fallback;
    ApplyEditorTheme();
    ImGui_ImplGlfw_InitForOpenGL(window, true); ImGui_ImplOpenGL3_Init("#version 330");

    struct Cell { UIElementType type; float w, h; std::function<void(UIElementComponent&)> setup; };
    std::vector<Cell> cells = {
        {UIElementType::Panel, 220, 120, {}},
        {UIElementType::InputField, 260, 36, [](auto& u) { u.inputText = "Player One"; }},
        {UIElementType::InputField, 260, 36, {}},
        {UIElementType::SearchField, 260, 36, [](auto& u) { u.inputText = "sword"; }},
        {UIElementType::Toggle, 220, 32, [](auto& u) { u.checked = true; }},
        {UIElementType::RadioGroup, 200, 100, {}},
        {UIElementType::ProgressBar, 260, 28, [](auto& u) { u.sliderValue = .65f; }},
        {UIElementType::ProgressBar, 260, 28, [](auto& u) { u.indeterminate = true; }},
        {UIElementType::RangeSlider, 260, 40, [](auto& u) { u.sliderValue = .2f; u.rangeUpper = .7f; }},
        {UIElementType::Separator, 260, 2, {}},
        {UIElementType::Scrollbar, 260, 14, [](auto& u) { u.vertical = false; u.sliderMax = 300; u.sliderValue = 80; }},
        {UIElementType::Dropdown, 220, 32, [](auto& u) { u.selectedIndex = 1; }},
        {UIElementType::EnumDropdown, 220, 32, [](auto& u) { u.selectedIndex = 0; }},
        {UIElementType::ListView, 220, 200, [](auto& u) { u.selectedIndex = 2; }},
        {UIElementType::GridView, 260, 200, [](auto& u) { u.selectedIndex = 4; }},
        {UIElementType::TreeView, 240, 200, [](auto& u) { u.selectedIndex = 4; u.expandedItems = {0, 1, 3}; }},
        {UIElementType::MultiColumnListView, 340, 200, [](auto& u) { u.selectedIndex = 1; }},
        {UIElementType::ReorderableList, 220, 200, [](auto& u) { u.selectedIndex = 1; }},
        {UIElementType::LogView, 340, 200, {}},
        {UIElementType::TabView, 300, 160, [](auto& u) { u.selectedIndex = 0; }},
        {UIElementType::Foldout, 260, 30, {}},
        {UIElementType::Toolbar, 300, 32, [](auto& u) { u.selectedIndex = 1; }},
        {UIElementType::Breadcrumb, 300, 28, {}},
        {UIElementType::MenuBar, 300, 30, {}},
        {UIElementType::PopupMenu, 200, 32, {}},
        {UIElementType::ColorPicker, 280, 300, [](auto& u) { u.numericValue = glm::vec4(.58f, .45f, .9f, 1); }},
        {UIElementType::NumericField, 200, 30, [](auto& u) { u.numericValue.x = 42.5f; }},
        {UIElementType::Vector2Field, 260, 30, [](auto& u) { u.numericValue = glm::vec4(1, 2, 0, 0); }},
        {UIElementType::Vector3Field, 320, 30, [](auto& u) { u.numericValue = glm::vec4(1, 2, 3, 0); }},
        {UIElementType::Vector4Field, 380, 30, [](auto& u) { u.numericValue = glm::vec4(1, 2, 3, 4); }},
        {UIElementType::ObjectField, 240, 30, {}},
        {UIElementType::SplitView, 300, 140, [](auto& u) { u.sliderValue = .4f; }},
        {UIElementType::PropertyGrid, 300, 200, {}},
        {UIElementType::FileBrowser, 340, 220, [](auto& u) { u.inputText = "Resources"; }},
        {UIElementType::Modal, 300, 150, {}},
        {UIElementType::Tooltip, 200, 40, [](auto& u) { u.tooltip = "Hover help"; }},
    };
    std::vector<SceneObject> scene;
    for (size_t i = 0; i < cells.size(); ++i) {
        scene.emplace_back(UIControls::TypeName(cells[i].type), ObjectType::UIImage, static_cast<int>(i));
        auto& obj = scene.back(); obj.id = static_cast<int>(i); obj.hasUI = true;
        UIControls::ApplyCreationDefaults(obj.ui, cells[i].type);
        obj.ui.size = glm::vec2(cells[i].w, cells[i].h);
        if (cells[i].setup) cells[i].setup(obj.ui);
    }
    scene[32].ui.linkedObjectId = 1; // property grid shows the input field
    scene[30].ui.linkedObjectId = 4;
    const bool interactive = !(argc > 2 && std::string(argv[2]) == "--edit");
    for (int frame = 0; frame < 6; ++frame) {
        glfwPollEvents(); ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
        io.DisplaySize = ImVec2((float)W, (float)H); io.DisplayFramebufferScale = ImVec2(1, 1);
        io.MousePos = ImVec2(-1, -1); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2((float)W, (float)H));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(17, 15, 24));
        ImGui::Begin("gallery", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleColor();
        auto* dl = ImGui::GetWindowDrawList();
        float x = 20, y = 20, rowH = 0;
        for (size_t i = 0; i < cells.size(); ++i) {
            const float w = cells[i].w, h = cells[i].h;
            if (x + w > W - 20) { x = 20; y += rowH + 22; rowH = 0; }
            dl->AddText(ImVec2(x, y), IM_COL32(150, 140, 175, 255), UIControls::TypeName(cells[i].type));
            ImVec2 min(x, y + 20), max(x + w, y + 20 + h);
            ImGui::PushID(static_cast<int>(i));
            UIControls::Draw(scene[i], scene, min, max, interactive);
            ImGui::PopID();
            x += w + 22; rowH = std::max(rowH, h + 20);
        }
        ImGui::End(); ImGui::Render(); const int width = W, height = H; glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height); glClearColor(.06f, .055f, .09f, 1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
        if (frame == 5) {
            std::vector<unsigned char> pixels(width * height * 3); glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream out(outPath, std::ios::binary); out << "P6\n" << width << " " << height << "\n255\n";
            for (int row = height - 1; row >= 0; --row) out.write(reinterpret_cast<char*>(pixels.data() + row * width * 3), width * 3);
        }
        glfwSwapBuffers(window);
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext(); glfwDestroyWindow(window); glfwTerminate();
    std::cout << "UI gallery written to " << outPath << "\n";
}
