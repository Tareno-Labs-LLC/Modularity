// The flat C API that C scripts (hand-written .c files and ModuCPP scripts the transpiler
// lowered to C) call into, implemented once here and exported from the editor and player.
//
// It used to be re-generated into every C script's own C++ wrapper (.capi.wrap.cpp), which
// pulled Engine.h and ImGui into each script compile and made the "fast" C path anything
// but. Now a C script compiles against ScriptRuntimeCAPI.h alone.
//
// ModuScriptContext keeps the layout the old per-script wrappers used ({ ScriptContext* })
// because the on-device Android editor still generates that wrapper for its TinyCC + Cling
// path: whichever definition a C object ends up bound to, the context means the same thing.
#include "ScriptRuntime.h"
#include "Engine.h"
#include "ScriptRuntimeCAPI.h"
#include "ModuCPPScriptApi.h"
#include "ThirdParty/ModuGUI/imgui.h"

#include <cstdio>
#include <string>

extern "C" {

struct ModuScriptContext {
    ScriptContext* ctx;
};

static ScriptContext* ModuAsCpp(ModuScriptContext* ctx) {
    return ctx ? ctx->ctx : nullptr;
}

static ModuVec3 ModuToVec3(const glm::vec3& v) {
    ModuVec3 out{v.x, v.y, v.z};
    return out;
}

static void ModuCopyToBuffer(const std::string& value, char* outBuffer, int outBufferSize) {
    if (!outBuffer || outBufferSize <= 0) return;
    std::snprintf(outBuffer, static_cast<size_t>(outBufferSize), "%s", value.c_str());
}

int Modu_GetObjectId(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->object) ? cpp->object->id : -1;
}

int Modu_IsObjectEnabled(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->IsObjectEnabled()) ? 1 : 0;
}

void Modu_SetObjectEnabled(ModuScriptContext* ctx, int enabled) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp) cpp->SetObjectEnabled(enabled != 0);
}

ModuVec3 Modu_GetPosition(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->object) ? ModuToVec3(cpp->object->position) : ModuVec3{0.0f, 0.0f, 0.0f};
}

ModuVec3 Modu_GetRotation(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->object) ? ModuToVec3(cpp->object->rotation) : ModuVec3{0.0f, 0.0f, 0.0f};
}

ModuVec3 Modu_GetScale(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->object) ? ModuToVec3(cpp->object->scale) : ModuVec3{1.0f, 1.0f, 1.0f};
}

void Modu_SetPosition(ModuScriptContext* ctx, ModuVec3 value) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp) cpp->SetPosition(glm::vec3(value.x, value.y, value.z));
}

void Modu_SetRotation(ModuScriptContext* ctx, ModuVec3 value) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp) cpp->SetRotation(glm::vec3(value.x, value.y, value.z));
}

void Modu_SetScale(ModuScriptContext* ctx, ModuVec3 value) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp) cpp->SetScale(glm::vec3(value.x, value.y, value.z));
}

int Modu_SetRigidbodyVelocity(ModuScriptContext* ctx, ModuVec3 velocity) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->SetRigidbodyVelocity(glm::vec3(velocity.x, velocity.y, velocity.z))) ? 1 : 0;
}

int Modu_AddRigidbodyForce(ModuScriptContext* ctx, ModuVec3 force) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->AddRigidbodyForce(glm::vec3(force.x, force.y, force.z))) ? 1 : 0;
}

int Modu_GetRigidbodyVelocity(ModuScriptContext* ctx, ModuVec3* outVelocity) {
    if (!outVelocity) return 0;
    *outVelocity = ModuVec3{0.0f, 0.0f, 0.0f};
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp) return 0;
    glm::vec3 vel(0.0f);
    if (!cpp->GetRigidbodyVelocity(vel)) return 0;
    *outVelocity = ModuToVec3(vel);
    return 1;
}

int Modu_SetRigidbodyRotation(ModuScriptContext* ctx, ModuVec3 rotation) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->SetRigidbodyRotation(glm::vec3(rotation.x, rotation.y, rotation.z))) ? 1 : 0;
}

float Modu_GetProjectGravityScale(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return cpp ? cpp->GetProjectGravityScale() : 1.0f;
}

void Modu_SetProjectGravityScale(ModuScriptContext* ctx, float scale) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp) cpp->SetProjectGravityScale(scale);
}

int Modu_EnsureCapsuleCollider(ModuScriptContext* ctx, float height, float radius) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->EnsureCapsuleCollider(height, radius)) ? 1 : 0;
}

int Modu_EnsureRigidbody(ModuScriptContext* ctx, int useGravity, int kinematic) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->EnsureRigidbody(useGravity != 0, kinematic != 0)) ? 1 : 0;
}

int Modu_HasAnimation(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->HasAnimation()) ? 1 : 0;
}

int Modu_PlayAnimation(ModuScriptContext* ctx, int restart) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->PlayAnimation(restart != 0)) ? 1 : 0;
}

int Modu_StopAnimation(ModuScriptContext* ctx, int resetTime) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->StopAnimation(resetTime != 0)) ? 1 : 0;
}

int Modu_PauseAnimation(ModuScriptContext* ctx, int pause) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->PauseAnimation(pause != 0)) ? 1 : 0;
}

int Modu_ReverseAnimation(ModuScriptContext* ctx, int restartIfStopped) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->ReverseAnimation(restartIfStopped != 0)) ? 1 : 0;
}

int Modu_SetAnimationTime(ModuScriptContext* ctx, float timeSeconds) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->SetAnimationTime(timeSeconds)) ? 1 : 0;
}

float Modu_GetAnimationTime(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return cpp ? cpp->GetAnimationTime() : 0.0f;
}

int Modu_IsAnimationPlaying(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->IsAnimationPlaying()) ? 1 : 0;
}

int Modu_SetAnimationLoop(ModuScriptContext* ctx, int loop) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->SetAnimationLoop(loop != 0)) ? 1 : 0;
}

int Modu_SetAnimationPlaySpeed(ModuScriptContext* ctx, float speed) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->SetAnimationPlaySpeed(speed)) ? 1 : 0;
}

int Modu_SetAnimationPlayOnAwake(ModuScriptContext* ctx, int playOnAwake) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->SetAnimationPlayOnAwake(playOnAwake != 0)) ? 1 : 0;
}

int Modu_IsSprintDown(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->IsSprintDown()) ? 1 : 0;
}

int Modu_IsJumpDown(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->IsJumpDown()) ? 1 : 0;
}

ModuVec3 Modu_GetMoveInputWASD(ModuScriptContext* ctx, float pitchDeg, float yawDeg) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return cpp ? ModuToVec3(cpp->GetMoveInputWASD(pitchDeg, yawDeg)) : ModuVec3{0.0f, 0.0f, 0.0f};
}

int Modu_ApplyMouseLook(ModuScriptContext* ctx, float* pitchDeg, float* yawDeg,
                        float sensitivity, float maxDelta, float deltaTime, int requireMouseButton) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp || !pitchDeg || !yawDeg) return 0;
    return cpp->ApplyMouseLook(*pitchDeg, *yawDeg, sensitivity, maxDelta, deltaTime,
                               requireMouseButton != 0) ? 1 : 0;
}

int Modu_RaycastClosestDetailed(ModuScriptContext* ctx, ModuVec3 origin, ModuVec3 dir, float distance,
                                ModuVec3* hitPos, ModuVec3* hitNormal, float* hitDistance,
                                int* hitObjectId, ModuVec3* hitObjectVelocity,
                                float* hitStaticFriction, float* hitDynamicFriction) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp) return 0;
    glm::vec3 hp(0.0f), hn(0.0f, 1.0f, 0.0f), hov(0.0f);
    float hd = 0.0f;
    int hid = -1;
    float hsf = 0.9f;
    float hdf = 0.9f;
    const bool ok = cpp->RaycastClosestDetailed(glm::vec3(origin.x, origin.y, origin.z),
                                                glm::vec3(dir.x, dir.y, dir.z), distance,
                                                hitPos ? &hp : nullptr,
                                                hitNormal ? &hn : nullptr,
                                                hitDistance ? &hd : nullptr,
                                                hitObjectId ? &hid : nullptr,
                                                hitObjectVelocity ? &hov : nullptr,
                                                hitStaticFriction ? &hsf : nullptr,
                                                hitDynamicFriction ? &hdf : nullptr);
    if (!ok) return 0;
    if (hitPos) *hitPos = ModuToVec3(hp);
    if (hitNormal) *hitNormal = ModuToVec3(hn);
    if (hitDistance) *hitDistance = hd;
    if (hitObjectId) *hitObjectId = hid;
    if (hitObjectVelocity) *hitObjectVelocity = ModuToVec3(hov);
    if (hitStaticFriction) *hitStaticFriction = hsf;
    if (hitDynamicFriction) *hitDynamicFriction = hdf;
    return 1;
}

void Modu_AddConsoleMessage(ModuScriptContext* ctx, const char* message, int type) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp) return;
    ConsoleMessageType msgType = ConsoleMessageType::Info;
    switch (type) {
        case MODU_CONSOLE_WARNING: msgType = ConsoleMessageType::Warning; break;
        case MODU_CONSOLE_ERROR: msgType = ConsoleMessageType::Error; break;
        case MODU_CONSOLE_SUCCESS: msgType = ConsoleMessageType::Success; break;
        default: msgType = ConsoleMessageType::Info; break;
    }
    cpp->AddConsoleMessage(message ? message : "", msgType);
}

float Modu_GetSettingFloat(ModuScriptContext* ctx, const char* key, float fallback) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp || !key) return fallback;
    return cpp->GetSettingFloat(key, fallback);
}

void Modu_SetSettingFloat(ModuScriptContext* ctx, const char* key, float value) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp && key) cpp->SetSettingFloat(key, value);
}

int Modu_GetSettingBool(ModuScriptContext* ctx, const char* key, int fallback) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp || !key) return fallback ? 1 : 0;
    return cpp->GetSettingBool(key, fallback != 0) ? 1 : 0;
}

void Modu_SetSettingBool(ModuScriptContext* ctx, const char* key, int value) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp && key) cpp->SetSettingBool(key, value != 0);
}

int Modu_GetSettingInt(ModuScriptContext* ctx, const char* key, int fallback) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp || !key) return fallback;
    const std::string raw = cpp->GetSetting(key, "");
    if (raw.empty()) return fallback;
    try { return std::stoi(raw); } catch (...) {}
    return fallback;
}

void Modu_SetSettingInt(ModuScriptContext* ctx, const char* key, int value) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp && key) cpp->SetSetting(key, std::to_string(value));
}

ModuVec3 Modu_GetSettingVec3(ModuScriptContext* ctx, const char* key, ModuVec3 fallback) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp || !key) return fallback;
    return ModuToVec3(cpp->GetSettingVec3(key, glm::vec3(fallback.x, fallback.y, fallback.z)));
}

void Modu_SetSettingVec3(ModuScriptContext* ctx, const char* key, ModuVec3 value) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp && key) cpp->SetSettingVec3(key, glm::vec3(value.x, value.y, value.z));
}

void Modu_SetSettingString(ModuScriptContext* ctx, const char* key, const char* value) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (cpp && key) cpp->SetSetting(key, value ? value : "");
}

int Modu_GetSettingString(ModuScriptContext* ctx, const char* key, const char* fallback,
                          char* outBuffer, int outBufferSize) {
    if (!outBuffer || outBufferSize <= 0) return 0;
    outBuffer[0] = '\0';
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp || !key) {
        if (!fallback) return 0;
        ModuCopyToBuffer(fallback, outBuffer, outBufferSize);
        return 1;
    }
    ModuCopyToBuffer(cpp->GetSetting(key, fallback ? fallback : ""), outBuffer, outBufferSize);
    return 1;
}

int Modu_GetSpriteClipCount(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return cpp ? cpp->GetSpriteClipCount() : 0;
}

int Modu_GetSpriteClipIndex(ModuScriptContext* ctx) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return cpp ? cpp->GetSpriteClipIndex() : -1;
}

int Modu_SetSpriteClipIndex(ModuScriptContext* ctx, int index) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->SetSpriteClipIndex(index)) ? 1 : 0;
}

int Modu_SetSpriteClipName(ModuScriptContext* ctx, const char* name) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    return (cpp && cpp->SetSpriteClipName(name ? name : "")) ? 1 : 0;
}

int Modu_GetSpriteClipName(ModuScriptContext* ctx, char* outBuffer, int outBufferSize) {
    if (!outBuffer || outBufferSize <= 0) return 0;
    outBuffer[0] = '\0';
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp) return 0;
    const std::string value = cpp->GetSpriteClipName();
    ModuCopyToBuffer(value, outBuffer, outBufferSize);
    return !value.empty() ? 1 : 0;
}

int Modu_GetSpriteClipNameAt(ModuScriptContext* ctx, int index, char* outBuffer, int outBufferSize) {
    if (!outBuffer || outBufferSize <= 0) return 0;
    outBuffer[0] = '\0';
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp) return 0;
    const std::string value = cpp->GetSpriteClipNameAt(index);
    ModuCopyToBuffer(value, outBuffer, outBufferSize);
    return !value.empty() ? 1 : 0;
}

void Modu_InspectorText(ModuScriptContext* ctx, const char* text) {
    (void)ctx;
    ImGui::TextUnformatted(text ? text : "");
}

void Modu_InspectorSeparator(ModuScriptContext* ctx) {
    (void)ctx;
    ImGui::Separator();
}

int Modu_InspectorDragFloat(ModuScriptContext* ctx, const char* label, float* value,
                            float speed, float minValue, float maxValue, const char* format) {
    (void)ctx;
    if (!label || !value) return 0;
    const char* fmt = (format && format[0]) ? format : "%.3f";
    return ImGui::DragFloat(label, value, speed, minValue, maxValue, fmt) ? 1 : 0;
}

int Modu_InspectorDragFloat2(ModuScriptContext* ctx, const char* label, float* value,
                             float speed, float minValue, float maxValue, const char* format) {
    (void)ctx;
    if (!label || !value) return 0;
    const char* fmt = (format && format[0]) ? format : "%.3f";
    return ImGui::DragFloat2(label, value, speed, minValue, maxValue, fmt) ? 1 : 0;
}

int Modu_InspectorDragFloat3(ModuScriptContext* ctx, const char* label, float* value,
                             float speed, float minValue, float maxValue, const char* format) {
    (void)ctx;
    if (!label || !value) return 0;
    const char* fmt = (format && format[0]) ? format : "%.3f";
    return ImGui::DragFloat3(label, value, speed, minValue, maxValue, fmt) ? 1 : 0;
}

int Modu_InspectorCheckbox(ModuScriptContext* ctx, const char* label, int* value) {
    (void)ctx;
    if (!label || !value) return 0;
    bool checked = (*value != 0);
    const bool changed = ImGui::Checkbox(label, &checked);
    *value = checked ? 1 : 0;
    return changed ? 1 : 0;
}

int Modu_InspectorObject(ModuScriptContext* ctx, const char* label, int* objectId) {
    if (!objectId) return 0;
    ScriptContext* cpp = ModuAsCpp(ctx);
    int currentId = *objectId;
    const std::string labelText = (label && label[0]) ? label : "Object";
    const std::string pickerId = "##ObjectPicker_" + labelText;
    const std::string useSelectedId = "Use Selected##ObjectPicker_" + labelText;
    const std::string clearId = "Clear##ObjectPicker_" + labelText;
    std::string display = "None";
    if (cpp && currentId >= 0) {
        if (SceneObject* current = cpp->FindObjectById(currentId)) {
            display = current->name + " (" + std::to_string(current->id) + ")";
        } else {
            currentId = -1;
        }
    }
    bool changed = false;
    ImGui::TextUnformatted(labelText.c_str());
    const bool comboOpen = ImGui::BeginCombo(pickerId.c_str(), display.c_str());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT")) {
            if (payload->DataSize == sizeof(int)) {
                currentId = *static_cast<const int*>(payload->Data);
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (comboOpen) {
        if (ImGui::Selectable("None", currentId < 0)) {
            currentId = -1;
            changed = true;
        }
        if (cpp && cpp->engine) {
            for (const SceneObject& candidate : cpp->engine->getSceneObjects()) {
                const std::string candidateLabel = candidate.name + " (" + std::to_string(candidate.id) + ")";
                if (ImGui::Selectable(candidateLabel.c_str(), candidate.id == currentId)) {
                    currentId = candidate.id;
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }
    if (cpp && ImGui::Button(useSelectedId.c_str())) {
        const int selected = cpp->GetSelectedObjectId();
        if (selected >= 0) {
            currentId = selected;
            changed = true;
        }
    }
    if (ImGui::Button(clearId.c_str()) && currentId >= 0) {
        currentId = -1;
        changed = true;
    }
    if (changed) *objectId = currentId;
    return changed ? 1 : 0;
}

int Modu_InspectorSliderFloat(ModuScriptContext* ctx, const char* label, float* value,
                              float minValue, float maxValue) {
    (void)ctx;
    if (!label || !value) return 0;
    return ModuGUI::SliderFloat(label, value, minValue, maxValue) ? 1 : 0;
}

int Modu_InspectorSliderFloat3(ModuScriptContext* ctx, const char* label, float* value,
                               float minValue, float maxValue, const char* format) {
    (void)ctx;
    if (!label || !value) return 0;
    const char* fmt = (format && format[0]) ? format : "%.3f";
    return ModuGUI::SliderFloat3(label, value, minValue, maxValue, fmt) ? 1 : 0;
}

int Modu_InspectorInputInt(ModuScriptContext* ctx, const char* label, int* value) {
    (void)ctx;
    if (!label || !value) return 0;
    return ModuGUI::InputInt(label, value) ? 1 : 0;
}

int Modu_InspectorSliderInt(ModuScriptContext* ctx, const char* label, int* value,
                            int minValue, int maxValue) {
    (void)ctx;
    if (!label || !value) return 0;
    return ModuGUI::SliderInt(label, value, minValue, maxValue) ? 1 : 0;
}

int Modu_InspectorInputText(ModuScriptContext* ctx, const char* label, char* buffer, int bufferSize) {
    (void)ctx;
    if (!label || !buffer || bufferSize <= 0) return 0;
    return ModuGUI::InputText(label, buffer, static_cast<size_t>(bufferSize)) ? 1 : 0;
}

int Modu_InspectorObjectRef(ModuScriptContext* ctx, const char* label, char* refBuffer, int refBufferSize) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp || !refBuffer || refBufferSize <= 0) return 0;
    std::string ref(refBuffer);
    if (!::ModuCPP::DrawObjectRefInput(*cpp, label, ref)) return 0;
    ModuCopyToBuffer(ref, refBuffer, refBufferSize);
    return 1;
}

int Modu_ResolveObjectRef(ModuScriptContext* ctx, const char* objectRef) {
    ScriptContext* cpp = ModuAsCpp(ctx);
    if (!cpp || !objectRef || !objectRef[0]) return -1;
    SceneObject* resolved = cpp->ResolveObjectRef(objectRef);
    return resolved ? resolved->id : -1;
}

} // extern "C"
