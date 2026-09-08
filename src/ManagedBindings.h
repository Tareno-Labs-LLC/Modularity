#pragma once

#include "ScriptRuntime.h"
#include <cstdint>

extern "C" {
int modu_ctx_get_object_id(ScriptContext* ctx);
int modu_ctx_is_object_enabled(ScriptContext* ctx);
void modu_ctx_set_object_enabled(ScriptContext* ctx, int enabled);
int modu_ctx_get_layer(ScriptContext* ctx);
void modu_ctx_set_layer(ScriptContext* ctx, int layer);
int modu_ctx_has_tag(ScriptContext* ctx, const char* tag);
int modu_ctx_is_in_layer(ScriptContext* ctx, int layer);
void modu_ctx_get_tag(ScriptContext* ctx, char* outBuffer, int outBufferSize);
void modu_ctx_set_tag(ScriptContext* ctx, const char* tag);
void modu_ctx_get_position(ScriptContext* ctx, float* x, float* y, float* z);
void modu_ctx_set_position(ScriptContext* ctx, float x, float y, float z);
void modu_ctx_set_position_2d(ScriptContext* ctx, float x, float y);
void modu_ctx_get_rotation(ScriptContext* ctx, float* x, float* y, float* z);
void modu_ctx_set_rotation(ScriptContext* ctx, float x, float y, float z);
void modu_ctx_get_scale(ScriptContext* ctx, float* x, float* y, float* z);
void modu_ctx_set_scale(ScriptContext* ctx, float x, float y, float z);
int modu_ctx_is_sprint_down(ScriptContext* ctx);
int modu_ctx_is_jump_down(ScriptContext* ctx);
void modu_ctx_get_move_input_wasd(ScriptContext* ctx, float pitchDeg, float yawDeg, float* x, float* y, float* z);
int modu_ctx_apply_mouse_look(ScriptContext* ctx, float* pitchDeg, float* yawDeg,
                              float sensitivity, float maxDelta, float deltaTime, int requireMouseButton);
int modu_ctx_has_rigidbody(ScriptContext* ctx);
int modu_ctx_has_rigidbody_2d(ScriptContext* ctx);
int modu_ctx_ensure_capsule_collider(ScriptContext* ctx, float height, float radius);
int modu_ctx_ensure_rigidbody(ScriptContext* ctx, int useGravity, int kinematic);
int modu_ctx_set_rigidbody_velocity(ScriptContext* ctx, float x, float y, float z);
int modu_ctx_get_rigidbody_velocity(ScriptContext* ctx, float* x, float* y, float* z);
int modu_ctx_set_rigidbody_2d_velocity(ScriptContext* ctx, float x, float y);
int modu_ctx_get_rigidbody_2d_velocity(ScriptContext* ctx, float* x, float* y);
int modu_ctx_add_rigidbody_velocity(ScriptContext* ctx, float x, float y, float z);
int modu_ctx_set_rigidbody_angular_velocity(ScriptContext* ctx, float x, float y, float z);
int modu_ctx_get_rigidbody_angular_velocity(ScriptContext* ctx, float* x, float* y, float* z);
int modu_ctx_add_rigidbody_force(ScriptContext* ctx, float x, float y, float z);
int modu_ctx_add_rigidbody_impulse(ScriptContext* ctx, float x, float y, float z);
int modu_ctx_add_rigidbody_torque(ScriptContext* ctx, float x, float y, float z);
int modu_ctx_add_rigidbody_angular_impulse(ScriptContext* ctx, float x, float y, float z);
int modu_ctx_set_rigidbody_yaw(ScriptContext* ctx, float yawDegrees);
int modu_ctx_set_rigidbody_rotation(ScriptContext* ctx, float x, float y, float z);
int modu_ctx_teleport_rigidbody(ScriptContext* ctx, float px, float py, float pz, float rx, float ry, float rz);
float modu_ctx_get_project_gravity_scale(ScriptContext* ctx);
void modu_ctx_set_project_gravity_scale(ScriptContext* ctx, float scale);
int modu_ctx_raycast_closest_detailed(ScriptContext* ctx, float ox, float oy, float oz,
                                      float dx, float dy, float dz, float distance,
                                      float* hitPosX, float* hitPosY, float* hitPosZ,
                                      float* hitNormalX, float* hitNormalY, float* hitNormalZ,
                                      float* hitDistance, int* hitObjectId,
                                      float* hitObjectVelX, float* hitObjectVelY, float* hitObjectVelZ,
                                      float* hitStaticFriction, float* hitDynamicFriction);
int modu_ctx_has_animation(ScriptContext* ctx);
int modu_ctx_play_animation(ScriptContext* ctx, int restart);
int modu_ctx_stop_animation(ScriptContext* ctx, int resetTime);
int modu_ctx_pause_animation(ScriptContext* ctx, int pause);
int modu_ctx_reverse_animation(ScriptContext* ctx, int restartIfStopped);
int modu_ctx_set_animation_time(ScriptContext* ctx, float timeSeconds);
float modu_ctx_get_animation_time(ScriptContext* ctx);
int modu_ctx_is_animation_playing(ScriptContext* ctx);
int modu_ctx_set_animation_loop(ScriptContext* ctx, int loop);
int modu_ctx_set_animation_play_speed(ScriptContext* ctx, float speed);
int modu_ctx_set_animation_play_on_awake(ScriptContext* ctx, int playOnAwake);
float modu_ctx_get_setting_float(ScriptContext* ctx, const char* key, float fallback);
int modu_ctx_get_setting_bool(ScriptContext* ctx, const char* key, int fallback);
void modu_ctx_get_setting_string(ScriptContext* ctx, const char* key, const char* fallback,
                                 char* outBuffer, int outBufferSize);
void modu_ctx_set_setting_float(ScriptContext* ctx, const char* key, float value);
void modu_ctx_set_setting_bool(ScriptContext* ctx, const char* key, int value);
void modu_ctx_set_setting_string(ScriptContext* ctx, const char* key, const char* value);
void modu_ctx_add_console_message(ScriptContext* ctx, const char* message, int type);
int modu_ctx_is_ui_button_pressed(ScriptContext* ctx);
int modu_ctx_is_ui_interactable(ScriptContext* ctx);
void modu_ctx_set_ui_interactable(ScriptContext* ctx, int interactable);
float modu_ctx_get_ui_slider_value(ScriptContext* ctx);
void modu_ctx_set_ui_slider_value(ScriptContext* ctx, float value);
void modu_ctx_set_ui_slider_range(ScriptContext* ctx, float minValue, float maxValue);
void modu_ctx_set_ui_label(ScriptContext* ctx, const char* label);
void modu_ctx_set_ui_color(ScriptContext* ctx, float r, float g, float b, float a);
float modu_ctx_get_ui_text_scale(ScriptContext* ctx);
void modu_ctx_set_ui_text_scale(ScriptContext* ctx, float scale);
void modu_ctx_set_ui_slider_style(ScriptContext* ctx, int style);
void modu_ctx_set_ui_button_style(ScriptContext* ctx, int style);
void modu_ctx_set_ui_style_preset(ScriptContext* ctx, const char* name);
void modu_ctx_set_fps_cap(ScriptContext* ctx, int enabled, float cap);
int modu_ctx_get_sprite_clip_count(ScriptContext* ctx);
int modu_ctx_get_sprite_clip_index(ScriptContext* ctx);
void modu_ctx_get_sprite_clip_name(ScriptContext* ctx, char* outBuffer, int outBufferSize);
void modu_ctx_get_sprite_clip_name_at(ScriptContext* ctx, int index, char* outBuffer, int outBufferSize);
int modu_ctx_set_sprite_clip_index(ScriptContext* ctx, int index);
int modu_ctx_set_sprite_clip_name(ScriptContext* ctx, const char* name);
float modu_ctx_get_sprite_alpha(ScriptContext* ctx);
void modu_ctx_set_sprite_alpha(ScriptContext* ctx, float alpha);
int modu_ctx_fade_sprite_alpha(ScriptContext* ctx, float targetAlpha, float duration, float deltaTime);
int modu_ctx_fade_sprite_to_clip_index(ScriptContext* ctx, int clipIndex,
                                       float fadeOutDuration, float fadeInDuration, float deltaTime);
int modu_ctx_fade_sprite_to_clip_name(ScriptContext* ctx, const char* clipName,
                                      float fadeOutDuration, float fadeInDuration, float deltaTime);
int modu_ctx_find_object_id_by_name(ScriptContext* ctx, const char* name);
void modu_ctx_get_object_name(ScriptContext* ctx, int id, char* outBuffer, int outBufferSize);
int modu_ctx_get_selected_object_id(ScriptContext* ctx);
int modu_ctx_get_scene_object_count(ScriptContext* ctx);
int modu_ctx_get_scene_object_id_at(ScriptContext* ctx, int index);
int modu_ctx_has_audio_source(ScriptContext* ctx);
int modu_ctx_play_audio(ScriptContext* ctx);
int modu_ctx_stop_audio(ScriptContext* ctx);
int modu_ctx_set_audio_loop(ScriptContext* ctx, int loop);
int modu_ctx_set_audio_volume(ScriptContext* ctx, float volume);
int modu_ctx_set_audio_clip(ScriptContext* ctx, const char* path);
int modu_ctx_play_audio_one_shot(ScriptContext* ctx, const char* clipPath, float volumeScale);
void modu_ctx_mark_dirty(ScriptContext* ctx);
void modu_imgui_text(const char* text);
void modu_imgui_separator();
int modu_imgui_button(const char* label);
int modu_imgui_checkbox(const char* label, int* value);
int modu_imgui_drag_float(const char* label, float* value, float speed, float minValue, float maxValue);
int modu_imgui_drag_float3(const char* label, float* values, float speed, float minValue, float maxValue);
int modu_imgui_input_text(const char* label, char* buffer, int bufferSize);
int modu_imgui_begin_combo(const char* label, const char* previewValue);
void modu_imgui_end_combo();
int modu_imgui_selectable(const char* label, int selected);
int modu_imgui_accept_scene_object_drop(int* outId);
}

struct ManagedNativeApi {
    uint32_t version = 1;
    int (*getObjectId)(ScriptContext* ctx) = nullptr;
    void (*getPosition)(ScriptContext* ctx, float* x, float* y, float* z) = nullptr;
    void (*setPosition)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    void (*getRotation)(ScriptContext* ctx, float* x, float* y, float* z) = nullptr;
    void (*setRotation)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    void (*getScale)(ScriptContext* ctx, float* x, float* y, float* z) = nullptr;
    void (*setScale)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    int (*hasRigidbody)(ScriptContext* ctx) = nullptr;
    int (*ensureRigidbody)(ScriptContext* ctx, int useGravity, int kinematic) = nullptr;
    int (*setRigidbodyVelocity)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    int (*getRigidbodyVelocity)(ScriptContext* ctx, float* x, float* y, float* z) = nullptr;
    int (*addRigidbodyForce)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    int (*addRigidbodyImpulse)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    float (*getSettingFloat)(ScriptContext* ctx, const char* key, float fallback) = nullptr;
    int (*getSettingBool)(ScriptContext* ctx, const char* key, int fallback) = nullptr;
    void (*getSettingString)(ScriptContext* ctx, const char* key, const char* fallback,
                             char* outBuffer, int outBufferSize) = nullptr;
    void (*setSettingFloat)(ScriptContext* ctx, const char* key, float value) = nullptr;
    void (*setSettingBool)(ScriptContext* ctx, const char* key, int value) = nullptr;
    void (*setSettingString)(ScriptContext* ctx, const char* key, const char* value) = nullptr;
    void (*addConsoleMessage)(ScriptContext* ctx, const char* message, int type) = nullptr;
    int (*findObjectIdByName)(ScriptContext* ctx, const char* name) = nullptr;
    void (*getObjectName)(ScriptContext* ctx, int id, char* outBuffer, int outBufferSize) = nullptr;
    int (*getSelectedObjectId)(ScriptContext* ctx) = nullptr;
    void (*imguiText)(const char* text) = nullptr;
    void (*imguiSeparator)() = nullptr;
    int (*imguiButton)(const char* label) = nullptr;
    int (*imguiCheckbox)(const char* label, int* value) = nullptr;
    int (*imguiDragFloat)(const char* label, float* value, float speed, float minValue, float maxValue) = nullptr;
    int (*imguiDragFloat3)(const char* label, float* values, float speed, float minValue, float maxValue) = nullptr;
    int (*imguiInputText)(const char* label, char* buffer, int bufferSize) = nullptr;
    int (*imguiAcceptSceneObjectDrop)(int* outId) = nullptr;
    // Version 4+ additions appended to preserve ABI layout for older managed assemblies.
    int (*getSceneObjectCount)(ScriptContext* ctx) = nullptr;
    int (*getSceneObjectIdAt)(ScriptContext* ctx, int index) = nullptr;
    int (*imguiBeginCombo)(const char* label, const char* previewValue) = nullptr;
    void (*imguiEndCombo)() = nullptr;
    int (*imguiSelectable)(const char* label, int selected) = nullptr;
    // Version 5+ additions.
    int (*hasAnimation)(ScriptContext* ctx) = nullptr;
    int (*playAnimation)(ScriptContext* ctx, int restart) = nullptr;
    int (*stopAnimation)(ScriptContext* ctx, int resetTime) = nullptr;
    int (*pauseAnimation)(ScriptContext* ctx, int pause) = nullptr;
    int (*reverseAnimation)(ScriptContext* ctx, int restartIfStopped) = nullptr;
    int (*setAnimationTime)(ScriptContext* ctx, float timeSeconds) = nullptr;
    float (*getAnimationTime)(ScriptContext* ctx) = nullptr;
    int (*isAnimationPlaying)(ScriptContext* ctx) = nullptr;
    int (*setAnimationLoop)(ScriptContext* ctx, int loop) = nullptr;
    int (*setAnimationPlaySpeed)(ScriptContext* ctx, float speed) = nullptr;
    int (*setAnimationPlayOnAwake)(ScriptContext* ctx, int playOnAwake) = nullptr;
    // Version 6+ additions.
    int (*isObjectEnabled)(ScriptContext* ctx) = nullptr;
    void (*setObjectEnabled)(ScriptContext* ctx, int enabled) = nullptr;
    int (*getLayer)(ScriptContext* ctx) = nullptr;
    void (*setLayer)(ScriptContext* ctx, int layer) = nullptr;
    int (*hasTag)(ScriptContext* ctx, const char* tag) = nullptr;
    int (*isInLayer)(ScriptContext* ctx, int layer) = nullptr;
    void (*getTag)(ScriptContext* ctx, char* outBuffer, int outBufferSize) = nullptr;
    void (*setTag)(ScriptContext* ctx, const char* tag) = nullptr;
    void (*setPosition2D)(ScriptContext* ctx, float x, float y) = nullptr;
    int (*isSprintDown)(ScriptContext* ctx) = nullptr;
    int (*isJumpDown)(ScriptContext* ctx) = nullptr;
    void (*getMoveInputWASD)(ScriptContext* ctx, float pitchDeg, float yawDeg, float* x, float* y, float* z) = nullptr;
    int (*applyMouseLook)(ScriptContext* ctx, float* pitchDeg, float* yawDeg,
                          float sensitivity, float maxDelta, float deltaTime, int requireMouseButton) = nullptr;
    int (*hasRigidbody2D)(ScriptContext* ctx) = nullptr;
    int (*setRigidbody2DVelocity)(ScriptContext* ctx, float x, float y) = nullptr;
    int (*getRigidbody2DVelocity)(ScriptContext* ctx, float* x, float* y) = nullptr;
    int (*addRigidbodyVelocity)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    int (*setRigidbodyAngularVelocity)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    int (*getRigidbodyAngularVelocity)(ScriptContext* ctx, float* x, float* y, float* z) = nullptr;
    int (*addRigidbodyTorque)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    int (*addRigidbodyAngularImpulse)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    int (*setRigidbodyYaw)(ScriptContext* ctx, float yawDegrees) = nullptr;
    int (*setRigidbodyRotation)(ScriptContext* ctx, float x, float y, float z) = nullptr;
    int (*teleportRigidbody)(ScriptContext* ctx, float px, float py, float pz, float rx, float ry, float rz) = nullptr;
    int (*raycastClosestDetailed)(ScriptContext* ctx, float ox, float oy, float oz,
                                  float dx, float dy, float dz, float distance,
                                  float* hitPosX, float* hitPosY, float* hitPosZ,
                                  float* hitNormalX, float* hitNormalY, float* hitNormalZ,
                                  float* hitDistance, int* hitObjectId,
                                  float* hitObjectVelX, float* hitObjectVelY, float* hitObjectVelZ,
                                  float* hitStaticFriction, float* hitDynamicFriction) = nullptr;
    int (*isUIButtonPressed)(ScriptContext* ctx) = nullptr;
    int (*isUIInteractable)(ScriptContext* ctx) = nullptr;
    void (*setUIInteractable)(ScriptContext* ctx, int interactable) = nullptr;
    float (*getUISliderValue)(ScriptContext* ctx) = nullptr;
    void (*setUISliderValue)(ScriptContext* ctx, float value) = nullptr;
    void (*setUISliderRange)(ScriptContext* ctx, float minValue, float maxValue) = nullptr;
    void (*setUILabel)(ScriptContext* ctx, const char* label) = nullptr;
    void (*setUIColor)(ScriptContext* ctx, float r, float g, float b, float a) = nullptr;
    float (*getUITextScale)(ScriptContext* ctx) = nullptr;
    void (*setUITextScale)(ScriptContext* ctx, float scale) = nullptr;
    void (*setUISliderStyle)(ScriptContext* ctx, int style) = nullptr;
    void (*setUIButtonStyle)(ScriptContext* ctx, int style) = nullptr;
    void (*setUIStylePreset)(ScriptContext* ctx, const char* name) = nullptr;
    void (*setFPSCap)(ScriptContext* ctx, int enabled, float cap) = nullptr;
    int (*getSpriteClipCount)(ScriptContext* ctx) = nullptr;
    int (*getSpriteClipIndex)(ScriptContext* ctx) = nullptr;
    void (*getSpriteClipName)(ScriptContext* ctx, char* outBuffer, int outBufferSize) = nullptr;
    void (*getSpriteClipNameAt)(ScriptContext* ctx, int index, char* outBuffer, int outBufferSize) = nullptr;
    int (*setSpriteClipIndex)(ScriptContext* ctx, int index) = nullptr;
    int (*setSpriteClipName)(ScriptContext* ctx, const char* name) = nullptr;
    float (*getSpriteAlpha)(ScriptContext* ctx) = nullptr;
    void (*setSpriteAlpha)(ScriptContext* ctx, float alpha) = nullptr;
    int (*fadeSpriteAlpha)(ScriptContext* ctx, float targetAlpha, float duration, float deltaTime) = nullptr;
    int (*fadeSpriteToClipIndex)(ScriptContext* ctx, int clipIndex,
                                 float fadeOutDuration, float fadeInDuration, float deltaTime) = nullptr;
    int (*fadeSpriteToClipName)(ScriptContext* ctx, const char* clipName,
                                float fadeOutDuration, float fadeInDuration, float deltaTime) = nullptr;
    int (*hasAudioSource)(ScriptContext* ctx) = nullptr;
    int (*playAudio)(ScriptContext* ctx) = nullptr;
    int (*stopAudio)(ScriptContext* ctx) = nullptr;
    int (*setAudioLoop)(ScriptContext* ctx, int loop) = nullptr;
    int (*setAudioVolume)(ScriptContext* ctx, float volume) = nullptr;
    int (*setAudioClip)(ScriptContext* ctx, const char* path) = nullptr;
    int (*playAudioOneShot)(ScriptContext* ctx, const char* clipPath, float volumeScale) = nullptr;
    void (*markDirty)(ScriptContext* ctx) = nullptr;
    int (*ensureCapsuleCollider)(ScriptContext* ctx, float height, float radius) = nullptr;
    // Version 7+ additions.
    float (*getProjectGravityScale)(ScriptContext* ctx) = nullptr;
    void (*setProjectGravityScale)(ScriptContext* ctx, float scale) = nullptr;
};

ManagedNativeApi BuildManagedNativeApi();
