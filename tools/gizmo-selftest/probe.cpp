// Render the production ModuGuizmo library in a hidden GL context and check
// projection, clip balance, shading, controller input and draw-only stereo state.
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "ThirdParty/ImGuizmo/ImGuizmo.h" // Also exercise the SDK compatibility header.
#include "ThirdParty/glm/glm.hpp"
#include "ThirdParty/glm/gtc/matrix_transform.hpp"
#include "ThirdParty/glm/gtc/type_ptr.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <set>
#include <vector>

static int failures = 0;
static void check(bool passed, const char* label)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    if (!passed) ++failures;
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool useGL = argc < 3 || std::strcmp(argv[2], "--headless") != 0;
    GLFWwindow* window = nullptr;
    if (useGL)
    {
        std::puts("Initializing hidden OpenGL context...");
        if (!glfwInit()) return 2;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        window = glfwCreateWindow(1000, 700, "ModuGuizmo probe", nullptr, nullptr);
        if (!window) return 3;
        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 4;
    }
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char* atlas = nullptr;
    int atlasWidth = 0, atlasHeight = 0;
    if (useGL)
    {
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");
    }
    else
    {
        io.Fonts->GetTexDataAsRGBA32(&atlas, &atlasWidth, &atlasHeight);
        io.Fonts->SetTexID((ImTextureID)1);
    }
    io.AddMousePosEvent(-100, -100);
    const glm::mat4 view = glm::lookAt(glm::vec3(3, 2, 4), glm::vec3(0), glm::vec3(0, 1, 0));
    const glm::mat4 projection = glm::perspective(glm::radians(40.f), 500.f / 350.f, .1f, 100.f);
    auto begin = [&] {
        if (useGL)
        {
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
        }
        io.DisplaySize = ImVec2(1000, 700);
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Probe", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGuizmo::BeginFrame();
        ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
        ImGuizmo::SetGizmoSizeClipSpace(.24f);
    };
    // Warm the renderer/font atlas before collecting draw data.
    begin(); ImGui::End(); ImGui::Render();
    if (useGL) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    begin();
    auto* draw = ImGui::GetWindowDrawList();
    std::vector<ImU32> referenceColors;
    const char* labels[] = {"Perspective / dark scene", "Perspective / bright scene", "Orthographic / local axes", "Universal / rotated axes"};
    for (int panel = 0; panel < 4; ++panel)
    {
        const float x = float(panel % 2) * 500.f;
        const float y = float(panel / 2) * 350.f;
        const bool bright = panel == 1;
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + 500, y + 350), bright ? IM_COL32(222, 225, 228, 255) : IM_COL32(38, 42, 49, 255));
        draw->AddText(ImVec2(x + 20, y + 18), bright ? IM_COL32(35, 38, 43, 255) : IM_COL32_WHITE, labels[panel]);
        for (int i = 1; i < 10; ++i)
            draw->AddLine(ImVec2(x + i * 50.f, y + 60), ImVec2(x + i * 50.f, y + 340), bright ? IM_COL32(201, 205, 210, 255) : IM_COL32(49, 54, 62, 255));
        glm::mat4 model(1.f);
        if (panel > 1) model = glm::rotate(model, .6f, glm::normalize(glm::vec3(1, 1, 0)));
        const glm::mat4 proj = panel == 2 ? glm::ortho(-2.8f, 2.8f, -1.96f, 1.96f, .1f, 100.f) : projection;
        ImGuizmo::SetID(panel);
        ImGuizmo::SetRect(x, y, 500, 350);
        ImGuizmo::SetOrthographic(panel == 2);
        const int clipDepth = draw->_ClipRectStack.Size;
        const int vertexStart = draw->VtxBuffer.Size;
        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), panel == 3 ? ImGuizmo::UNIVERSAL : ImGuizmo::TRANSLATE,
            ImGuizmo::LOCAL, glm::value_ptr(model));
        check(draw->_ClipRectStack.Size == clipDepth, "visible gizmo balances its clip rectangle");
        check(draw->VtxBuffer.Size - vertexStart > 500, "solid handle geometry is emitted");
        std::vector<ImU32> colors;
        std::set<ImU32> shades;
        for (int i = vertexStart; i < draw->VtxBuffer.Size; ++i)
        {
            const auto& vertex = draw->VtxBuffer[i];
            if (!std::isfinite(vertex.pos.x) || !std::isfinite(vertex.pos.y)) ++failures;
            colors.push_back(vertex.col);
            shades.insert(vertex.col);
        }
        check(shades.size() > 30, "handle normals produce varied lighting");
        if (panel == 0) referenceColors = colors;
        if (panel == 1) check(colors == referenceColors, "lighting is identical against dark and bright scenes");
        const glm::mat4 before = model;
        const bool usingBefore = ImGuizmo::IsUsing();
        ImGuizmo::DrawGizmo(glm::value_ptr(view), glm::value_ptr(proj), ImGuizmo::TRANSLATE, ImGuizmo::LOCAL, glm::value_ptr(model));
        check(model == before && ImGuizmo::IsUsing() == usingBefore, "stereo draw-only pass preserves transform and interaction");
    }
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(0, 0, 500, 350);
    const auto backwards = glm::lookAt(glm::vec3(0, 0, 4), glm::vec3(0, 0, 8), glm::vec3(0, 1, 0));
    glm::mat4 model(1.f);
    const int clipDepth = draw->_ClipRectStack.Size;
    ImGuizmo::Manipulate(glm::value_ptr(backwards), glm::value_ptr(projection), ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(model));
    check(draw->_ClipRectStack.Size == clipDepth, "behind-camera early return balances clipping");
    ImGui::End(); ImGui::Render();
    if (useGL)
    {
        glViewport(0, 0, 1000, 700);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glFinish();
    }
    if (argc > 1)
    {
        std::vector<unsigned char> pixels(1000 * 700 * 3);
        if (useGL)
        {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, 1000, 700, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        }
        else
        {
            // Software rasterize the actual ImGui output for environments where
            // the window system or graphics driver cannot create a GL context.
            for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
                for (const ImDrawCmd& cmd : list->CmdBuffer)
                    for (unsigned int index = 0; index + 2 < cmd.ElemCount; index += 3)
                    {
                        if (cmd.UserCallback) continue;
                        const auto& a = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + index]];
                        const auto& b = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + index + 1]];
                        const auto& c = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + index + 2]];
                        const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
                        if (std::abs(area) < .0001f) continue;
                        const int minX = ImMax(0, int(ImMax(cmd.ClipRect.x, ImMin(a.pos.x, ImMin(b.pos.x, c.pos.x)))));
                        const int maxX = ImMin(999, int(ImMin(cmd.ClipRect.z, ImMax(a.pos.x, ImMax(b.pos.x, c.pos.x)))));
                        const int minY = ImMax(0, int(ImMax(cmd.ClipRect.y, ImMin(a.pos.y, ImMin(b.pos.y, c.pos.y)))));
                        const int maxY = ImMin(699, int(ImMin(cmd.ClipRect.w, ImMax(a.pos.y, ImMax(b.pos.y, c.pos.y)))));
                        const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a.col), cb = ImGui::ColorConvertU32ToFloat4(b.col), cc = ImGui::ColorConvertU32ToFloat4(c.col);
                        for (int y = minY; y <= maxY; ++y) for (int x = minX; x <= maxX; ++x)
                        {
                            const float px = x + .5f, py = y + .5f;
                            const float wa = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
                            const float wb = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
                            const float wc = 1.f - wa - wb;
                            if (wa < 0 || wb < 0 || wc < 0) continue;
                            const int tx = ImClamp(int((a.uv.x * wa + b.uv.x * wb + c.uv.x * wc) * atlasWidth), 0, atlasWidth - 1);
                            const int ty = ImClamp(int((a.uv.y * wa + b.uv.y * wb + c.uv.y * wc) * atlasHeight), 0, atlasHeight - 1);
                            const float alpha = (ca.w * wa + cb.w * wb + cc.w * wc) * atlas[(ty * atlasWidth + tx) * 4 + 3] / 255.f;
                            unsigned char* dest = pixels.data() + ((699 - y) * 1000 + x) * 3;
                            const float rgb[] = {ca.x * wa + cb.x * wb + cc.x * wc, ca.y * wa + cb.y * wb + cc.y * wc, ca.z * wa + cb.z * wb + cc.z * wc};
                            for (int channel = 0; channel < 3; ++channel)
                                dest[channel] = (unsigned char)(rgb[channel] * 255.f * alpha + dest[channel] * (1.f - alpha));
                        }
                    }
        }
        std::ofstream output(argv[1], std::ios::binary);
        output << "P6\n1000 700\n255\n";
        for (int y = 699; y >= 0; --y) output.write((char*)pixels.data() + y * 3000, 3000);
    }
    if (useGL) check(glGetError() == GL_NO_ERROR, "render finishes without OpenGL errors");
    // Controller ray owns interaction; move it while holding the screen handle.
    const auto frontView = glm::lookAt(glm::vec3(0, 0, 4), glm::vec3(0), glm::vec3(0, 1, 0));
    const float rayDirection[] = {0, 0, -1};
    for (int frame = 0; frame < 4; ++frame)
    {
        io.AddMousePosEvent(250, 175);
        io.AddMouseButtonEvent(0, frame == 1 || frame == 2);
        begin();
        ImGuizmo::SetID(42);
        ImGuizmo::SetRect(0, 0, 500, 350);
        const float rayOrigin[] = {frame >= 2 ? .4f : 0.f, 0, 4};
        ImGuizmo::SetInputRay(rayOrigin, rayDirection);
        ImGuizmo::Manipulate(glm::value_ptr(frontView), glm::value_ptr(projection), ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(model));
        ImGuizmo::SetInputRay(nullptr, nullptr);
        ImGui::End(); ImGui::Render();
    }
    check(model[3].x > .3f, "controller ray drag translates the model");
    check(!ImGuizmo::IsUsing(), "controller release ends the drag");
    if (useGL)
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }
    ImGui::DestroyContext();
    if (useGL)
    {
        glfwDestroyWindow(window);
        glfwTerminate();
    }
    std::printf("ModuGuizmo probe: %d failures\n", failures);
    return failures ? 1 : 0;
}
