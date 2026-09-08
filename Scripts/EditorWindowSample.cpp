#include "ScriptRuntime.h"
#include "SceneObject.h"
#include "ThirdParty/ModuGUI/imgui.h"

namespace {

bool clampFps = false;
float moveStep = 0.25f;
float rotateStep = 15.0f;
float scaleStep = 0.10f;
char note[128] = "Hello from native C++";

float Clamp(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

float Max(float a, float b) {
    return a > b ? a : b;
}

void BindSettings(ScriptContext& ctx) {
    ctx.AutoSetting("clampFps", clampFps);
    ctx.AutoSetting("moveStep", moveStep);
    ctx.AutoSetting("rotateStep", rotateStep);
    ctx.AutoSetting("scaleStep", scaleStep);
    ctx.AutoSetting("note", note, sizeof(note));
}

void DrawEditorWindow(ScriptContext& ctx) {
    BindSettings(ctx);

    bool changed = false;

    ImGui::TextUnformatted("Editor Window Sample");
    ImGui::Separator();

    changed |= ImGui::Checkbox("Clamp FPS To 120", &clampFps);
    changed |= ImGui::DragFloat("Move Step", &moveStep, 0.01f, 0.01f, 10.0f, "%.2f");
    changed |= ImGui::DragFloat("Rotate Step", &rotateStep, 1.0f, 1.0f, 180.0f, "%.1f");
    changed |= ImGui::DragFloat("Scale Step", &scaleStep, 0.01f, 0.01f, 2.0f, "%.2f");
    changed |= ImGui::InputText("Log Message", note, sizeof(note));

    if (changed) {
        moveStep = Clamp(moveStep, 0.01f, 10.0f);
        rotateStep = Clamp(rotateStep, 1.0f, 180.0f);
        scaleStep = Clamp(scaleStep, 0.01f, 2.0f);
        ctx.SaveAutoSettings();
    }

    if (clampFps) {
        ctx.SetFPSCap(true, 120.0f);
    }

    if (ImGui::Button("Log To Console")) {
        ctx.AddConsoleMessage(std::string("EditorWindowSample.cpp: ") + note);
    }

    ImGui::Separator();

    if (!ctx.object) {
        ImGui::TextDisabled("Select an object to use the transform tools.");
        return;
    }

    SceneObject* selected = ctx.object;
    ImGui::TextDisabled("Selected object: %s (id=%d)", selected->name.c_str(), selected->id);

    glm::vec3 position = selected->position;
    if (ImGui::DragFloat3("Position", &position.x, moveStep, -10000.0f, 10000.0f, "%.2f")) {
        ctx.SetPosition(position);
        ctx.MarkDirty();
    }

    glm::vec3 rotation = selected->rotation;
    if (ImGui::DragFloat3("Rotation", &rotation.x, rotateStep, -3600.0f, 3600.0f, "%.1f")) {
        ctx.SetRotation(rotation);
        ctx.MarkDirty();
    }

    glm::vec3 scale = selected->scale;
    if (ImGui::DragFloat3("Scale", &scale.x, scaleStep, 0.01f, 1000.0f, "%.2f")) {
        scale.x = Max(scale.x, 0.01f);
        scale.y = Max(scale.y, 0.01f);
        scale.z = Max(scale.z, 0.01f);
        ctx.SetScale(scale);
        ctx.MarkDirty();
    }

    if (ImGui::Button("Nudge +Y")) {
        position = selected->position;
        position.y += moveStep;
        ctx.SetPosition(position);
        ctx.MarkDirty();
    }

    ImGui::SameLine();
    if (ImGui::Button("Rotate +Y")) {
        rotation = selected->rotation;
        rotation.y += rotateStep;
        ctx.SetRotation(rotation);
        ctx.MarkDirty();
    }

    ImGui::SameLine();
    if (ImGui::Button("Grow Uniform")) {
        scale = selected->scale + glm::vec3(scaleStep);
        scale.x = Max(scale.x, 0.01f);
        scale.y = Max(scale.y, 0.01f);
        scale.z = Max(scale.z, 0.01f);
        ctx.SetScale(scale);
        ctx.MarkDirty();
    }

    if (ImGui::Button("Reset Local Transform")) {
        ctx.SetPosition(glm::vec3(0.0f));
        ctx.SetRotation(glm::vec3(0.0f));
        ctx.SetScale(glm::vec3(1.0f));
        ctx.MarkDirty();
    }
}

} // namespace

extern "C" void RenderEditorWindow(ScriptContext& ctx) {
    DrawEditorWindow(ctx);
}

extern "C" void ExitRenderEditorWindow(ScriptContext& ctx) {
    (void)ctx;
}
