#include "ManagedBindings.h"
#include "Engine.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
    void modu_copy_string(const std::string& value, char* outBuffer, int outBufferSize) {
        if (!outBuffer || outBufferSize <= 0) return;
        std::snprintf(outBuffer, static_cast<size_t>(outBufferSize), "%s", value.c_str());
    }
}

int modu_ctx_get_object_id(ScriptContext* ctx) {
    return (ctx && ctx->object) ? ctx->object->id : -1;
}

int modu_ctx_is_object_enabled(ScriptContext* ctx) {
    return (ctx && ctx->IsObjectEnabled()) ? 1 : 0;
}

void modu_ctx_set_object_enabled(ScriptContext* ctx, int enabled) {
    if (!ctx) return;
    ctx->SetObjectEnabled(enabled != 0);
}

int modu_ctx_get_layer(ScriptContext* ctx) {
    if (!ctx) return 0;
    return ctx->GetLayer();
}

void modu_ctx_set_layer(ScriptContext* ctx, int layer) {
    if (!ctx) return;
    ctx->SetLayer(layer);
}

int modu_ctx_has_tag(ScriptContext* ctx, const char* tag) {
    if (!ctx || !tag) return 0;
    return ctx->HasTag(tag) ? 1 : 0;
}

int modu_ctx_is_in_layer(ScriptContext* ctx, int layer) {
    return (ctx && ctx->IsInLayer(layer)) ? 1 : 0;
}

void modu_ctx_get_tag(ScriptContext* ctx, char* outBuffer, int outBufferSize) {
    if (!ctx) {
        modu_copy_string("", outBuffer, outBufferSize);
        return;
    }
    modu_copy_string(ctx->GetTag(), outBuffer, outBufferSize);
}

void modu_ctx_set_tag(ScriptContext* ctx, const char* tag) {
    if (!ctx) return;
    ctx->SetTag(tag ? tag : "");
}

void modu_ctx_get_position(ScriptContext* ctx, float* x, float* y, float* z) {
    if (!ctx || !ctx->object || !x || !y || !z) return;
    *x = ctx->object->position.x;
    *y = ctx->object->position.y;
    *z = ctx->object->position.z;
}

void modu_ctx_set_position(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return;
    ctx->SetPosition(glm::vec3(x, y, z));
}

void modu_ctx_set_position_2d(ScriptContext* ctx, float x, float y) {
    if (!ctx) return;
    ctx->SetPosition2D(glm::vec2(x, y));
}

void modu_ctx_get_rotation(ScriptContext* ctx, float* x, float* y, float* z) {
    if (!ctx || !ctx->object || !x || !y || !z) return;
    *x = ctx->object->rotation.x;
    *y = ctx->object->rotation.y;
    *z = ctx->object->rotation.z;
}

void modu_ctx_set_rotation(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return;
    ctx->SetRotation(glm::vec3(x, y, z));
}

void modu_ctx_get_scale(ScriptContext* ctx, float* x, float* y, float* z) {
    if (!ctx || !ctx->object || !x || !y || !z) return;
    *x = ctx->object->scale.x;
    *y = ctx->object->scale.y;
    *z = ctx->object->scale.z;
}

void modu_ctx_set_scale(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return;
    ctx->SetScale(glm::vec3(x, y, z));
}

int modu_ctx_is_sprint_down(ScriptContext* ctx) {
    return (ctx && ctx->IsSprintDown()) ? 1 : 0;
}

int modu_ctx_is_jump_down(ScriptContext* ctx) {
    return (ctx && ctx->IsJumpDown()) ? 1 : 0;
}

void modu_ctx_get_move_input_wasd(ScriptContext* ctx, float pitchDeg, float yawDeg, float* x, float* y, float* z) {
    if (!x || !y || !z) return;
    *x = 0.0f;
    *y = 0.0f;
    *z = 0.0f;
    if (!ctx) return;
    glm::vec3 move = ctx->GetMoveInputWASD(pitchDeg, yawDeg);
    *x = move.x;
    *y = move.y;
    *z = move.z;
}

int modu_ctx_apply_mouse_look(ScriptContext* ctx, float* pitchDeg, float* yawDeg,
                              float sensitivity, float maxDelta, float deltaTime, int requireMouseButton) {
    if (!ctx || !pitchDeg || !yawDeg) return 0;
    return ctx->ApplyMouseLook(*pitchDeg, *yawDeg, sensitivity, maxDelta, deltaTime, requireMouseButton != 0) ? 1 : 0;
}

int modu_ctx_has_rigidbody(ScriptContext* ctx) {
    return (ctx && ctx->HasRigidbody()) ? 1 : 0;
}

int modu_ctx_has_rigidbody_2d(ScriptContext* ctx) {
    return (ctx && ctx->HasRigidbody2D()) ? 1 : 0;
}

int modu_ctx_ensure_capsule_collider(ScriptContext* ctx, float height, float radius) {
    if (!ctx) return 0;
    return ctx->EnsureCapsuleCollider(height, radius) ? 1 : 0;
}

int modu_ctx_ensure_rigidbody(ScriptContext* ctx, int useGravity, int kinematic) {
    if (!ctx) return 0;
    return ctx->EnsureRigidbody(useGravity != 0, kinematic != 0) ? 1 : 0;
}

int modu_ctx_set_rigidbody_velocity(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return 0;
    return ctx->SetRigidbodyVelocity(glm::vec3(x, y, z)) ? 1 : 0;
}

int modu_ctx_get_rigidbody_velocity(ScriptContext* ctx, float* x, float* y, float* z) {
    if (!ctx || !x || !y || !z) return 0;
    glm::vec3 velocity(0.0f);
    if (!ctx->GetRigidbodyVelocity(velocity)) return 0;
    *x = velocity.x;
    *y = velocity.y;
    *z = velocity.z;
    return 1;
}

int modu_ctx_set_rigidbody_2d_velocity(ScriptContext* ctx, float x, float y) {
    if (!ctx) return 0;
    return ctx->SetRigidbody2DVelocity(glm::vec2(x, y)) ? 1 : 0;
}

int modu_ctx_get_rigidbody_2d_velocity(ScriptContext* ctx, float* x, float* y) {
    if (!ctx || !x || !y) return 0;
    glm::vec2 velocity(0.0f);
    if (!ctx->GetRigidbody2DVelocity(velocity)) return 0;
    *x = velocity.x;
    *y = velocity.y;
    return 1;
}

int modu_ctx_add_rigidbody_velocity(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return 0;
    return ctx->AddRigidbodyVelocity(glm::vec3(x, y, z)) ? 1 : 0;
}

int modu_ctx_set_rigidbody_angular_velocity(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return 0;
    return ctx->SetRigidbodyAngularVelocity(glm::vec3(x, y, z)) ? 1 : 0;
}

int modu_ctx_get_rigidbody_angular_velocity(ScriptContext* ctx, float* x, float* y, float* z) {
    if (!ctx || !x || !y || !z) return 0;
    glm::vec3 velocity(0.0f);
    if (!ctx->GetRigidbodyAngularVelocity(velocity)) return 0;
    *x = velocity.x;
    *y = velocity.y;
    *z = velocity.z;
    return 1;
}

int modu_ctx_add_rigidbody_force(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return 0;
    return ctx->AddRigidbodyForce(glm::vec3(x, y, z)) ? 1 : 0;
}

int modu_ctx_add_rigidbody_impulse(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return 0;
    return ctx->AddRigidbodyImpulse(glm::vec3(x, y, z)) ? 1 : 0;
}

int modu_ctx_add_rigidbody_torque(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return 0;
    return ctx->AddRigidbodyTorque(glm::vec3(x, y, z)) ? 1 : 0;
}

int modu_ctx_add_rigidbody_angular_impulse(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return 0;
    return ctx->AddRigidbodyAngularImpulse(glm::vec3(x, y, z)) ? 1 : 0;
}

int modu_ctx_set_rigidbody_yaw(ScriptContext* ctx, float yawDegrees) {
    if (!ctx) return 0;
    return ctx->SetRigidbodyYaw(yawDegrees) ? 1 : 0;
}

int modu_ctx_set_rigidbody_rotation(ScriptContext* ctx, float x, float y, float z) {
    if (!ctx) return 0;
    return ctx->SetRigidbodyRotation(glm::vec3(x, y, z)) ? 1 : 0;
}

int modu_ctx_teleport_rigidbody(ScriptContext* ctx, float px, float py, float pz, float rx, float ry, float rz) {
    if (!ctx) return 0;
    return ctx->TeleportRigidbody(glm::vec3(px, py, pz), glm::vec3(rx, ry, rz)) ? 1 : 0;
}

float modu_ctx_get_project_gravity_scale(ScriptContext* ctx) {
    if (!ctx) return 1.0f;
    return ctx->GetProjectGravityScale();
}

void modu_ctx_set_project_gravity_scale(ScriptContext* ctx, float scale) {
    if (!ctx) return;
    ctx->SetProjectGravityScale(scale);
}

int modu_ctx_raycast_closest_detailed(ScriptContext* ctx, float ox, float oy, float oz,
                                      float dx, float dy, float dz, float distance,
                                      float* hitPosX, float* hitPosY, float* hitPosZ,
                                      float* hitNormalX, float* hitNormalY, float* hitNormalZ,
                                      float* hitDistance, int* hitObjectId,
                                      float* hitObjectVelX, float* hitObjectVelY, float* hitObjectVelZ,
                                      float* hitStaticFriction, float* hitDynamicFriction) {
    if (!ctx) return 0;
    glm::vec3 hitPos(0.0f);
    glm::vec3 hitNormal(0.0f);
    glm::vec3 hitVelocity(0.0f);
    float dist = 0.0f;
    int objectId = -1;
    float staticFriction = 0.0f;
    float dynamicFriction = 0.0f;
    bool hit = ctx->RaycastClosestDetailed(glm::vec3(ox, oy, oz), glm::vec3(dx, dy, dz), distance,
                                           &hitPos, &hitNormal, &dist, &objectId, &hitVelocity,
                                           &staticFriction, &dynamicFriction);
    if (!hit) return 0;
    if (hitPosX) *hitPosX = hitPos.x;
    if (hitPosY) *hitPosY = hitPos.y;
    if (hitPosZ) *hitPosZ = hitPos.z;
    if (hitNormalX) *hitNormalX = hitNormal.x;
    if (hitNormalY) *hitNormalY = hitNormal.y;
    if (hitNormalZ) *hitNormalZ = hitNormal.z;
    if (hitDistance) *hitDistance = dist;
    if (hitObjectId) *hitObjectId = objectId;
    if (hitObjectVelX) *hitObjectVelX = hitVelocity.x;
    if (hitObjectVelY) *hitObjectVelY = hitVelocity.y;
    if (hitObjectVelZ) *hitObjectVelZ = hitVelocity.z;
    if (hitStaticFriction) *hitStaticFriction = staticFriction;
    if (hitDynamicFriction) *hitDynamicFriction = dynamicFriction;
    return 1;
}

int modu_ctx_has_animation(ScriptContext* ctx) {
    return (ctx && ctx->HasAnimation()) ? 1 : 0;
}

int modu_ctx_play_animation(ScriptContext* ctx, int restart) {
    if (!ctx) return 0;
    return ctx->PlayAnimation(restart != 0) ? 1 : 0;
}

int modu_ctx_stop_animation(ScriptContext* ctx, int resetTime) {
    if (!ctx) return 0;
    return ctx->StopAnimation(resetTime != 0) ? 1 : 0;
}

int modu_ctx_pause_animation(ScriptContext* ctx, int pause) {
    if (!ctx) return 0;
    return ctx->PauseAnimation(pause != 0) ? 1 : 0;
}

int modu_ctx_reverse_animation(ScriptContext* ctx, int restartIfStopped) {
    if (!ctx) return 0;
    return ctx->ReverseAnimation(restartIfStopped != 0) ? 1 : 0;
}

int modu_ctx_set_animation_time(ScriptContext* ctx, float timeSeconds) {
    if (!ctx) return 0;
    return ctx->SetAnimationTime(timeSeconds) ? 1 : 0;
}

float modu_ctx_get_animation_time(ScriptContext* ctx) {
    if (!ctx) return 0.0f;
    return ctx->GetAnimationTime();
}

int modu_ctx_is_animation_playing(ScriptContext* ctx) {
    return (ctx && ctx->IsAnimationPlaying()) ? 1 : 0;
}

int modu_ctx_set_animation_loop(ScriptContext* ctx, int loop) {
    if (!ctx) return 0;
    return ctx->SetAnimationLoop(loop != 0) ? 1 : 0;
}

int modu_ctx_set_animation_play_speed(ScriptContext* ctx, float speed) {
    if (!ctx) return 0;
    return ctx->SetAnimationPlaySpeed(speed) ? 1 : 0;
}

int modu_ctx_set_animation_play_on_awake(ScriptContext* ctx, int playOnAwake) {
    if (!ctx) return 0;
    return ctx->SetAnimationPlayOnAwake(playOnAwake != 0) ? 1 : 0;
}

float modu_ctx_get_setting_float(ScriptContext* ctx, const char* key, float fallback) {
    if (!ctx || !key) return fallback;
    return ctx->GetSettingFloat(key, fallback);
}

int modu_ctx_get_setting_bool(ScriptContext* ctx, const char* key, int fallback) {
    if (!ctx || !key) return fallback ? 1 : 0;
    return ctx->GetSettingBool(key, fallback != 0) ? 1 : 0;
}

void modu_ctx_get_setting_string(ScriptContext* ctx, const char* key, const char* fallback,
                                 char* outBuffer, int outBufferSize) {
    if (!outBuffer || outBufferSize <= 0) return;
    std::string value;
    if (!ctx || !key) {
        value = fallback ? fallback : "";
    } else {
        value = ctx->GetSetting(key, fallback ? fallback : "");
    }
    std::snprintf(outBuffer, static_cast<size_t>(outBufferSize), "%s", value.c_str());
}

void modu_ctx_set_setting_float(ScriptContext* ctx, const char* key, float value) {
    if (!ctx || !key) return;
    ctx->SetSettingFloat(key, value);
}

void modu_ctx_set_setting_bool(ScriptContext* ctx, const char* key, int value) {
    if (!ctx || !key) return;
    ctx->SetSettingBool(key, value != 0);
}

void modu_ctx_set_setting_string(ScriptContext* ctx, const char* key, const char* value) {
    if (!ctx || !key) return;
    ctx->SetSetting(key, value ? value : "");
}

void modu_ctx_add_console_message(ScriptContext* ctx, const char* message, int type) {
    if (!ctx || !message) return;
    ctx->AddConsoleMessage(message, static_cast<ConsoleMessageType>(type));
}

int modu_ctx_is_ui_button_pressed(ScriptContext* ctx) {
    return (ctx && ctx->IsUIButtonPressed()) ? 1 : 0;
}

int modu_ctx_is_ui_interactable(ScriptContext* ctx) {
    return (ctx && ctx->IsUIInteractable()) ? 1 : 0;
}

void modu_ctx_set_ui_interactable(ScriptContext* ctx, int interactable) {
    if (!ctx) return;
    ctx->SetUIInteractable(interactable != 0);
}

float modu_ctx_get_ui_slider_value(ScriptContext* ctx) {
    if (!ctx) return 0.0f;
    return ctx->GetUISliderValue();
}

void modu_ctx_set_ui_slider_value(ScriptContext* ctx, float value) {
    if (!ctx) return;
    ctx->SetUISliderValue(value);
}

void modu_ctx_set_ui_slider_range(ScriptContext* ctx, float minValue, float maxValue) {
    if (!ctx) return;
    ctx->SetUISliderRange(minValue, maxValue);
}

void modu_ctx_set_ui_label(ScriptContext* ctx, const char* label) {
    if (!ctx) return;
    ctx->SetUILabel(label ? label : "");
}

void modu_ctx_set_ui_color(ScriptContext* ctx, float r, float g, float b, float a) {
    if (!ctx) return;
    ctx->SetUIColor(glm::vec4(r, g, b, a));
}

float modu_ctx_get_ui_text_scale(ScriptContext* ctx) {
    if (!ctx) return 1.0f;
    return ctx->GetUITextScale();
}

void modu_ctx_set_ui_text_scale(ScriptContext* ctx, float scale) {
    if (!ctx) return;
    ctx->SetUITextScale(scale);
}

void modu_ctx_set_ui_slider_style(ScriptContext* ctx, int style) {
    if (!ctx) return;
    ctx->SetUISliderStyle(static_cast<UISliderStyle>(style));
}

void modu_ctx_set_ui_button_style(ScriptContext* ctx, int style) {
    if (!ctx) return;
    ctx->SetUIButtonStyle(static_cast<UIButtonStyle>(style));
}

void modu_ctx_set_ui_style_preset(ScriptContext* ctx, const char* name) {
    if (!ctx) return;
    ctx->SetUIStylePreset(name ? name : "");
}

void modu_ctx_set_fps_cap(ScriptContext* ctx, int enabled, float cap) {
    if (!ctx) return;
    ctx->SetFPSCap(enabled != 0, cap);
}

int modu_ctx_get_sprite_clip_count(ScriptContext* ctx) {
    if (!ctx) return 0;
    return ctx->GetSpriteClipCount();
}

int modu_ctx_get_sprite_clip_index(ScriptContext* ctx) {
    if (!ctx) return -1;
    return ctx->GetSpriteClipIndex();
}

void modu_ctx_get_sprite_clip_name(ScriptContext* ctx, char* outBuffer, int outBufferSize) {
    if (!ctx) {
        modu_copy_string("", outBuffer, outBufferSize);
        return;
    }
    modu_copy_string(ctx->GetSpriteClipName(), outBuffer, outBufferSize);
}

void modu_ctx_get_sprite_clip_name_at(ScriptContext* ctx, int index, char* outBuffer, int outBufferSize) {
    if (!ctx) {
        modu_copy_string("", outBuffer, outBufferSize);
        return;
    }
    modu_copy_string(ctx->GetSpriteClipNameAt(index), outBuffer, outBufferSize);
}

int modu_ctx_set_sprite_clip_index(ScriptContext* ctx, int index) {
    if (!ctx) return 0;
    return ctx->SetSpriteClipIndex(index) ? 1 : 0;
}

int modu_ctx_set_sprite_clip_name(ScriptContext* ctx, const char* name) {
    if (!ctx || !name) return 0;
    return ctx->SetSpriteClipName(name) ? 1 : 0;
}

float modu_ctx_get_sprite_alpha(ScriptContext* ctx) {
    if (!ctx) return 1.0f;
    return ctx->GetSpriteAlpha();
}

void modu_ctx_set_sprite_alpha(ScriptContext* ctx, float alpha) {
    if (!ctx) return;
    ctx->SetSpriteAlpha(alpha);
}

int modu_ctx_fade_sprite_alpha(ScriptContext* ctx, float targetAlpha, float duration, float deltaTime) {
    if (!ctx) return 0;
    return ctx->FadeSpriteAlpha(targetAlpha, duration, deltaTime) ? 1 : 0;
}

int modu_ctx_fade_sprite_to_clip_index(ScriptContext* ctx, int clipIndex,
                                       float fadeOutDuration, float fadeInDuration, float deltaTime) {
    if (!ctx) return 0;
    return ctx->FadeSpriteToClipIndex(clipIndex, fadeOutDuration, fadeInDuration, deltaTime) ? 1 : 0;
}

int modu_ctx_fade_sprite_to_clip_name(ScriptContext* ctx, const char* clipName,
                                      float fadeOutDuration, float fadeInDuration, float deltaTime) {
    if (!ctx || !clipName) return 0;
    return ctx->FadeSpriteToClipName(clipName, fadeOutDuration, fadeInDuration, deltaTime) ? 1 : 0;
}

int modu_ctx_find_object_id_by_name(ScriptContext* ctx, const char* name) {
    if (!ctx || !name) return -1;
    SceneObject* obj = ctx->FindObjectByName(name);
    return obj ? obj->id : -1;
}

void modu_ctx_get_object_name(ScriptContext* ctx, int id, char* outBuffer, int outBufferSize) {
    if (!outBuffer || outBufferSize <= 0) return;
    const char* name = "";
    if (ctx) {
        SceneObject* obj = ctx->FindObjectById(id);
        if (obj) name = obj->name.c_str();
    }
    std::snprintf(outBuffer, static_cast<size_t>(outBufferSize), "%s", name);
}

int modu_ctx_get_selected_object_id(ScriptContext* ctx) {
    if (!ctx) return -1;
    return ctx->GetSelectedObjectId();
}

int modu_ctx_get_scene_object_count(ScriptContext* ctx) {
    if (!ctx) return 0;
    return ctx->GetSceneObjectCount();
}

int modu_ctx_get_scene_object_id_at(ScriptContext* ctx, int index) {
    if (!ctx) return -1;
    return ctx->GetSceneObjectIdAt(index);
}

int modu_ctx_has_audio_source(ScriptContext* ctx) {
    return (ctx && ctx->HasAudioSource()) ? 1 : 0;
}

int modu_ctx_play_audio(ScriptContext* ctx) {
    if (!ctx) return 0;
    return ctx->PlayAudio() ? 1 : 0;
}

int modu_ctx_stop_audio(ScriptContext* ctx) {
    if (!ctx) return 0;
    return ctx->StopAudio() ? 1 : 0;
}

int modu_ctx_set_audio_loop(ScriptContext* ctx, int loop) {
    if (!ctx) return 0;
    return ctx->SetAudioLoop(loop != 0) ? 1 : 0;
}

int modu_ctx_set_audio_volume(ScriptContext* ctx, float volume) {
    if (!ctx) return 0;
    return ctx->SetAudioVolume(volume) ? 1 : 0;
}

int modu_ctx_set_audio_clip(ScriptContext* ctx, const char* path) {
    if (!ctx) return 0;
    return ctx->SetAudioClip(path ? path : "") ? 1 : 0;
}

int modu_ctx_play_audio_one_shot(ScriptContext* ctx, const char* clipPath, float volumeScale) {
    if (!ctx) return 0;
    return ctx->PlayAudioOneShot(clipPath ? clipPath : "", volumeScale) ? 1 : 0;
}

void modu_ctx_mark_dirty(ScriptContext* ctx) {
    if (!ctx) return;
    ctx->MarkDirty();
}

void modu_imgui_text(const char* text) {
    if (!text) return;
    ImGui::TextUnformatted(text);
}

void modu_imgui_separator() {
    ImGui::Separator();
}

int modu_imgui_button(const char* label) {
    if (!label) return 0;
    return ImGui::Button(label) ? 1 : 0;
}

int modu_imgui_checkbox(const char* label, int* value) {
    if (!label || !value) return 0;
    bool checked = (*value != 0);
    bool changed = ImGui::Checkbox(label, &checked);
    *value = checked ? 1 : 0;
    return changed ? 1 : 0;
}

int modu_imgui_drag_float(const char* label, float* value, float speed, float minValue, float maxValue) {
    if (!label || !value) return 0;
    return ImGui::DragFloat(label, value, speed, minValue, maxValue) ? 1 : 0;
}

int modu_imgui_drag_float3(const char* label, float* values, float speed, float minValue, float maxValue) {
    if (!label || !values) return 0;
    return ImGui::DragFloat3(label, values, speed, minValue, maxValue) ? 1 : 0;
}

int modu_imgui_input_text(const char* label, char* buffer, int bufferSize) {
    if (!label || !buffer || bufferSize <= 0) return 0;
    return ImGui::InputText(label, buffer, static_cast<size_t>(bufferSize)) ? 1 : 0;
}

int modu_imgui_begin_combo(const char* label, const char* previewValue) {
    if (!label) return 0;
    return ImGui::BeginCombo(label, previewValue ? previewValue : "") ? 1 : 0;
}

void modu_imgui_end_combo() {
    ImGui::EndCombo();
}

int modu_imgui_selectable(const char* label, int selected) {
    if (!label) return 0;
    return ImGui::Selectable(label, selected != 0) ? 1 : 0;
}

int modu_imgui_accept_scene_object_drop(int* outId) {
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT")) {
            if (outId && payload->DataSize == sizeof(int)) {
                *outId = *(const int*)payload->Data;
            }
            ImGui::EndDragDropTarget();
            return 1;
        }
        ImGui::EndDragDropTarget();
    }
    return 0;
}

ManagedNativeApi BuildManagedNativeApi() {
    ManagedNativeApi api;
    api.version = 7;
    api.getObjectId = modu_ctx_get_object_id;
    api.getPosition = modu_ctx_get_position;
    api.setPosition = modu_ctx_set_position;
    api.getRotation = modu_ctx_get_rotation;
    api.setRotation = modu_ctx_set_rotation;
    api.getScale = modu_ctx_get_scale;
    api.setScale = modu_ctx_set_scale;
    api.hasRigidbody = modu_ctx_has_rigidbody;
    api.ensureRigidbody = modu_ctx_ensure_rigidbody;
    api.setRigidbodyVelocity = modu_ctx_set_rigidbody_velocity;
    api.getRigidbodyVelocity = modu_ctx_get_rigidbody_velocity;
    api.addRigidbodyForce = modu_ctx_add_rigidbody_force;
    api.addRigidbodyImpulse = modu_ctx_add_rigidbody_impulse;
    api.getSettingFloat = modu_ctx_get_setting_float;
    api.getSettingBool = modu_ctx_get_setting_bool;
    api.getSettingString = modu_ctx_get_setting_string;
    api.setSettingFloat = modu_ctx_set_setting_float;
    api.setSettingBool = modu_ctx_set_setting_bool;
    api.setSettingString = modu_ctx_set_setting_string;
    api.addConsoleMessage = modu_ctx_add_console_message;
    api.findObjectIdByName = modu_ctx_find_object_id_by_name;
    api.getObjectName = modu_ctx_get_object_name;
    api.getSelectedObjectId = modu_ctx_get_selected_object_id;
    api.imguiText = modu_imgui_text;
    api.imguiSeparator = modu_imgui_separator;
    api.imguiButton = modu_imgui_button;
    api.imguiCheckbox = modu_imgui_checkbox;
    api.imguiDragFloat = modu_imgui_drag_float;
    api.imguiDragFloat3 = modu_imgui_drag_float3;
    api.imguiInputText = modu_imgui_input_text;
    api.imguiAcceptSceneObjectDrop = modu_imgui_accept_scene_object_drop;
    api.getSceneObjectCount = modu_ctx_get_scene_object_count;
    api.getSceneObjectIdAt = modu_ctx_get_scene_object_id_at;
    api.imguiBeginCombo = modu_imgui_begin_combo;
    api.imguiEndCombo = modu_imgui_end_combo;
    api.imguiSelectable = modu_imgui_selectable;
    api.hasAnimation = modu_ctx_has_animation;
    api.playAnimation = modu_ctx_play_animation;
    api.stopAnimation = modu_ctx_stop_animation;
    api.pauseAnimation = modu_ctx_pause_animation;
    api.reverseAnimation = modu_ctx_reverse_animation;
    api.setAnimationTime = modu_ctx_set_animation_time;
    api.getAnimationTime = modu_ctx_get_animation_time;
    api.isAnimationPlaying = modu_ctx_is_animation_playing;
    api.setAnimationLoop = modu_ctx_set_animation_loop;
    api.setAnimationPlaySpeed = modu_ctx_set_animation_play_speed;
    api.setAnimationPlayOnAwake = modu_ctx_set_animation_play_on_awake;
    api.isObjectEnabled = modu_ctx_is_object_enabled;
    api.setObjectEnabled = modu_ctx_set_object_enabled;
    api.getLayer = modu_ctx_get_layer;
    api.setLayer = modu_ctx_set_layer;
    api.hasTag = modu_ctx_has_tag;
    api.isInLayer = modu_ctx_is_in_layer;
    api.getTag = modu_ctx_get_tag;
    api.setTag = modu_ctx_set_tag;
    api.setPosition2D = modu_ctx_set_position_2d;
    api.isSprintDown = modu_ctx_is_sprint_down;
    api.isJumpDown = modu_ctx_is_jump_down;
    api.getMoveInputWASD = modu_ctx_get_move_input_wasd;
    api.applyMouseLook = modu_ctx_apply_mouse_look;
    api.hasRigidbody2D = modu_ctx_has_rigidbody_2d;
    api.setRigidbody2DVelocity = modu_ctx_set_rigidbody_2d_velocity;
    api.getRigidbody2DVelocity = modu_ctx_get_rigidbody_2d_velocity;
    api.addRigidbodyVelocity = modu_ctx_add_rigidbody_velocity;
    api.setRigidbodyAngularVelocity = modu_ctx_set_rigidbody_angular_velocity;
    api.getRigidbodyAngularVelocity = modu_ctx_get_rigidbody_angular_velocity;
    api.addRigidbodyTorque = modu_ctx_add_rigidbody_torque;
    api.addRigidbodyAngularImpulse = modu_ctx_add_rigidbody_angular_impulse;
    api.setRigidbodyYaw = modu_ctx_set_rigidbody_yaw;
    api.setRigidbodyRotation = modu_ctx_set_rigidbody_rotation;
    api.teleportRigidbody = modu_ctx_teleport_rigidbody;
    api.raycastClosestDetailed = modu_ctx_raycast_closest_detailed;
    api.isUIButtonPressed = modu_ctx_is_ui_button_pressed;
    api.isUIInteractable = modu_ctx_is_ui_interactable;
    api.setUIInteractable = modu_ctx_set_ui_interactable;
    api.getUISliderValue = modu_ctx_get_ui_slider_value;
    api.setUISliderValue = modu_ctx_set_ui_slider_value;
    api.setUISliderRange = modu_ctx_set_ui_slider_range;
    api.setUILabel = modu_ctx_set_ui_label;
    api.setUIColor = modu_ctx_set_ui_color;
    api.getUITextScale = modu_ctx_get_ui_text_scale;
    api.setUITextScale = modu_ctx_set_ui_text_scale;
    api.setUISliderStyle = modu_ctx_set_ui_slider_style;
    api.setUIButtonStyle = modu_ctx_set_ui_button_style;
    api.setUIStylePreset = modu_ctx_set_ui_style_preset;
    api.setFPSCap = modu_ctx_set_fps_cap;
    api.getSpriteClipCount = modu_ctx_get_sprite_clip_count;
    api.getSpriteClipIndex = modu_ctx_get_sprite_clip_index;
    api.getSpriteClipName = modu_ctx_get_sprite_clip_name;
    api.getSpriteClipNameAt = modu_ctx_get_sprite_clip_name_at;
    api.setSpriteClipIndex = modu_ctx_set_sprite_clip_index;
    api.setSpriteClipName = modu_ctx_set_sprite_clip_name;
    api.getSpriteAlpha = modu_ctx_get_sprite_alpha;
    api.setSpriteAlpha = modu_ctx_set_sprite_alpha;
    api.fadeSpriteAlpha = modu_ctx_fade_sprite_alpha;
    api.fadeSpriteToClipIndex = modu_ctx_fade_sprite_to_clip_index;
    api.fadeSpriteToClipName = modu_ctx_fade_sprite_to_clip_name;
    api.hasAudioSource = modu_ctx_has_audio_source;
    api.playAudio = modu_ctx_play_audio;
    api.stopAudio = modu_ctx_stop_audio;
    api.setAudioLoop = modu_ctx_set_audio_loop;
    api.setAudioVolume = modu_ctx_set_audio_volume;
    api.setAudioClip = modu_ctx_set_audio_clip;
    api.playAudioOneShot = modu_ctx_play_audio_one_shot;
    api.markDirty = modu_ctx_mark_dirty;
    api.ensureCapsuleCollider = modu_ctx_ensure_capsule_collider;
    api.getProjectGravityScale = modu_ctx_get_project_gravity_scale;
    api.setProjectGravityScale = modu_ctx_set_project_gravity_scale;
    return api;
}
