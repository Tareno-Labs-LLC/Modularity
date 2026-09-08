#pragma once

#include "ModuCPPScriptApi.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdio>
#include <vector>

namespace ModuCPP {

namespace Field {
inline float Float(const std::string& key, float fallback = 0.0f) {
    return GetSettingFloat(key, fallback);
}

inline int Int(const std::string& key, int fallback = 0) {
    const std::string raw = GetSetting(key, std::to_string(fallback));
    return raw.empty() ? fallback : std::atoi(raw.c_str());
}

inline bool Bool(const std::string& key, bool fallback = false) {
    return GetSettingBool(key, fallback);
}

inline std::string Text(const std::string& key, const std::string& fallback = "") {
    return GetSetting(key, fallback);
}

inline int Clip(const std::string& keyPrefix, int direction, int fallback = -1) {
    return Int(keyPrefix + std::to_string(direction), fallback);
}

inline int Clip(const std::string& keyPrefix, int direction, int frame, int fallback) {
    return Int(keyPrefix + std::to_string(direction) + "_" + std::to_string(frame), fallback);
}

inline void SetInt(const std::string& key, int value) {
    SetSetting(key, std::to_string(value));
}

inline void SetClip(const std::string& keyPrefix, int direction, int value) {
    SetInt(keyPrefix + std::to_string(direction), value);
}

inline void SetClip(const std::string& keyPrefix, int direction, int frame, int value) {
    SetInt(keyPrefix + std::to_string(direction) + "_" + std::to_string(frame), value);
}

// sheet-relative Sprite handles, keyed like Clip so Sprite[N]/[N][M] line up with BindArray.
// return type is qualified ::Sprite since this function shares the type's name.
inline ::Sprite Sprite(const std::string& key) {
    return DeserializeSprite(Text(key));
}

inline ::Sprite Sprite(const std::string& keyPrefix, int direction) {
    return DeserializeSprite(Text(keyPrefix + std::to_string(direction)));
}

inline ::Sprite Sprite(const std::string& keyPrefix, int direction, int frame) {
    return DeserializeSprite(Text(keyPrefix + std::to_string(direction) + "_" + std::to_string(frame)));
}

inline void SetSprite(const std::string& key, const ::Sprite& value) {
    SetSetting(key, SerializeSprite(value));
}

inline void SetSprite(const std::string& keyPrefix, int direction, const ::Sprite& value) {
    SetSetting(keyPrefix + std::to_string(direction), SerializeSprite(value));
}

inline void SetSprite(const std::string& keyPrefix, int direction, int frame, const ::Sprite& value) {
    SetSetting(keyPrefix + std::to_string(direction) + "_" + std::to_string(frame), SerializeSprite(value));
}
} // namespace Field

// Assign a sheet-relative Sprite to an object's sprite component. Target by id,
// by handle, or self (Scene::Current()). Lowering target for `obj.Sprite = expr`.
inline bool SetObjectSprite(int objectId, const ::Sprite& sprite) {
    if (ScriptContext* c = ctxPtr()) return c->SetObjectSprite(objectId, sprite);
    return false;
}

inline bool SetObjectSprite(SceneObject* object, const ::Sprite& sprite) {
    return object ? SetObjectSprite(object->id, sprite) : false;
}

namespace Scene {
inline SceneObject* Current() {
    if (ScriptContext* script = ctxPtr()) return script->object;
    return nullptr;
}

inline SceneObject* Resolve(const std::string& ref) {
    if (ScriptContext* script = ctxPtr()) return script->ResolveObjectRef(ref);
    return nullptr;
}

inline int Count() {
    if (ScriptContext* script = ctxPtr()) return script->GetSceneObjectCount();
    return 0;
}

inline const SceneObject* At(int index) {
    if (ScriptContext* script = ctxPtr()) return script->GetSceneObjectAt(index);
    return nullptr;
}
} // namespace Scene

namespace Physics {
inline bool ResolveGround(float capsuleHalf, float probeExtra, float groundSnap, float verticalVelocity, bool& grounded) {
    if (ScriptContext* script = ctxPtr()) return script->ResolveGround(capsuleHalf, probeExtra, groundSnap, verticalVelocity, nullptr, &grounded);
    grounded = false;
    return false;
}

inline bool RaycastClosestDetailed(const Vector3& origin, const Vector3& direction, float distance, Vector3& hitPos, Vector3& hitNormal, float& hitDistance, int& hitObjectId) {
    if (ScriptContext* script = ctxPtr()) return script->RaycastClosestDetailed(origin, direction, distance, &hitPos, &hitNormal, &hitDistance, &hitObjectId);
    hitObjectId = -1;
    return false;
}
} // namespace Physics

namespace ObjectAudio {
inline bool PlayOneShot(const std::string& refOrPath, float volumeScale = 1.0f) {
    if (refOrPath.empty()) return false;
    ScriptContext* script = ctxPtr();
    if (!script) return false;
    SceneObject* source = script->ResolveObjectRef(refOrPath);
    if (source && source->hasAudioSource && !source->audioSource.clipPath.empty()) return script->PlayObjectAudioOneShot(source->id, source->audioSource.clipPath, volumeScale);
    return script->PlayAudioOneShot(refOrPath, volumeScale);
}

inline bool PlayLoop(SceneObject* source) {
    ScriptContext* script = ctxPtr();
    if (!script || !source || !source->hasAudioSource || !source->audioSource.enabled || source->audioSource.clipPath.empty()) return false;
    return script->PlayObjectAudio(source->id);
}

inline void Stop(SceneObject* source) {
    if (ScriptContext* script = ctxPtr()) {
        if (source && source->hasAudioSource) script->StopObjectAudio(source->id);
    }
}

inline void SetLoop(SceneObject* source, bool loop) {
    if (ScriptContext* script = ctxPtr()) {
        if (source && source->hasAudioSource) script->SetObjectAudioLoop(source->id, loop);
    }
}
} // namespace ObjectAudio

namespace CameraMath {
inline void PlanarYawPitchVectors(float pitchDeg, float yawDeg, Vector3& forward, Vector3& right) {
    if (ScriptContext* script = ctxPtr()) {
        script->GetPlanarYawPitchVectors(pitchDeg, yawDeg, forward, right);
        return;
    }
    const glm::quat q = glm::quat(glm::radians(glm::vec3(pitchDeg, yawDeg, 0.0f)));
    forward = glm::normalize(q * glm::vec3(0.0f, 0.0f, -1.0f));
    right = glm::normalize(glm::vec3(-forward.z, 0.0f, forward.x));
}
} // namespace CameraMath

namespace detail {
inline bool isAudioPath(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == "wav" || ext == "mp3" || ext == "ogg" || ext == "flac" ||
           ext == "aac" || ext == "m4a";
}

inline bool isValidClip(const ScriptContext& ctx, int clipIndex) {
    return clipIndex >= 0 && clipIndex < ctx.GetSpriteClipCount();
}
} // namespace detail

inline float GetProjectGravityScale() {
    if (ScriptContext* scriptCtx = ctxPtr()) {
        return scriptCtx->GetProjectGravityScale();
    }
    return 1.0f;
}

inline void SetProjectGravityScale(float scale) {
    if (ScriptContext* scriptCtx = ctxPtr()) {
        scriptCtx->SetProjectGravityScale(scale);
    }
}

inline bool EditFloat(const char* label, float& value, float speed = 0.1f,
                      float minValue = 0.0f, float maxValue = 0.0f,
                      const char* format = "%.2f", const char* keyOverride = nullptr) {
    ScriptContext& scriptCtx = ctx();
    const std::string key = keyOverride ? keyOverride : detail::settingKeyFromLabel(label);
    scriptCtx.AutoSetting(key, value);
    const bool changed = ModuGUI::DragFloat(label, &value, speed, minValue, maxValue, format);
    if (changed) {
        scriptCtx.SaveAutoSettings();
    }
    return changed;
}

inline bool EditVec3(const char* label, vec3& value, float speed = 0.1f,
                     float minValue = 0.0f, float maxValue = 0.0f,
                     const char* format = "%.2f", const char* keyOverride = nullptr) {
    ScriptContext& scriptCtx = ctx();
    const std::string key = keyOverride ? keyOverride : detail::settingKeyFromLabel(label);
    scriptCtx.AutoSetting(key, value);
    const bool changed = ModuGUI::DragFloat3(label, &value.x, speed, minValue, maxValue, format);
    if (changed) {
        scriptCtx.SaveAutoSettings();
    }
    return changed;
}

inline bool EditBool(const char* label, bool& value, const char* keyOverride = nullptr) {
    ScriptContext& scriptCtx = ctx();
    const std::string key = keyOverride ? keyOverride : detail::settingKeyFromLabel(label);
    scriptCtx.AutoSetting(key, value);
    const bool changed = ModuGUI::Checkbox(label, &value);
    if (changed) {
        scriptCtx.SaveAutoSettings();
    }
    return changed;
}

inline bool EditInt(const char* label, int& value, int step = 1,
                    int stepFast = 100, const char* keyOverride = nullptr) {
    ScriptContext& scriptCtx = ctx();
    const std::string key = keyOverride ? keyOverride : detail::settingKeyFromLabel(label);
    scriptCtx.AutoSetting(key, value);
    const bool changed = ModuGUI::InputInt(label, &value, step, stepFast);
    if (changed) {
        scriptCtx.SaveAutoSettings();
    }
    return changed;
}

inline bool EditString(const char* label, std::string& value,
                       size_t capacity = 512, const char* keyOverride = nullptr,
                       ImGuiInputTextFlags flags = 0,
                       bool multiline = false,
                       float multilineHeight = 90.0f) {
    ScriptContext& scriptCtx = ctx();
    const std::string key = keyOverride ? keyOverride : detail::settingKeyFromLabel(label);
    scriptCtx.AutoSetting(key, value);

    std::vector<char> buffer(capacity + 1, '\0');
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());

    bool changed = false;
    if (multiline) {
        changed = ModuGUI::InputTextMultiline(label, buffer.data(), buffer.size(),
                                            ImVec2(-FLT_MIN, multilineHeight), flags);
    } else {
        changed = ModuGUI::InputText(label, buffer.data(), buffer.size(), flags);
    }

    if (changed) {
        value = buffer.data();
        scriptCtx.SaveAutoSettings();
    }

    return changed;
}

inline bool hasRigidbody2D(ScriptContext& ctx) {
    return ctx.HasRigidbody2D();
}

inline bool hasRigidbody2D() {
    if (ScriptContext* scriptCtx = ctxPtr()) return hasRigidbody2D(*scriptCtx);
    return false;
}

inline vec2 getRigidbody2DVelocity(ScriptContext& ctx) {
    vec2 velocity(0.0f);
    ctx.GetRigidbody2DVelocity(velocity);
    return velocity;
}

inline vec2 getRigidbody2DVelocity() {
    if (ScriptContext* scriptCtx = ctxPtr()) return getRigidbody2DVelocity(*scriptCtx);
    return vec2(0.0f);
}

inline bool setRigidbody2DVelocity(ScriptContext& ctx, const vec2& velocity) {
    return ctx.SetRigidbody2DVelocity(velocity);
}

inline bool setRigidbody2DVelocity(const vec2& velocity) {
    if (ScriptContext* scriptCtx = ctxPtr()) return setRigidbody2DVelocity(*scriptCtx, velocity);
    return false;
}

inline vec2 moveTowards(const vec2& current, const vec2& target, float maxDelta) {
    if (maxDelta <= 0.0f) return current;
    vec2 delta = target - current;
    const float len = glm::length(delta);
    if (len <= maxDelta || len <= 1e-6f) {
        return target;
    }
    return current + (delta / len) * maxDelta;
}

inline bool TryMoveRigidbody2D(ScriptContext& ctx,
                               const vec2& targetVelocity, float acceleration, float drag, float dt,
                               vec2* outVelocity = nullptr) {
    if (!hasRigidbody2D(ctx)) return false;

    vec2 velocity = getRigidbody2DVelocity(ctx);
    if (acceleration <= 0.0f) {
        velocity = targetVelocity;
    } else {
        velocity = moveTowards(velocity, targetVelocity, acceleration * dt);
    }

    if (glm::length(targetVelocity) < 1e-4f && drag > 0.0f) {
        const float damp = std::max(0.0f, 1.0f - drag * dt);
        velocity *= damp;
    }

    setRigidbody2DVelocity(ctx, velocity);
    if (outVelocity) {
        *outVelocity = velocity;
    }
    return true;
}

inline bool TryMoveRigidbody2D(ScriptContext& ctx,
                               const vec2& targetVelocity, float acceleration, float drag, float dt,
                               vec2& outVelocity) {
    return TryMoveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt, &outVelocity);
}

inline bool TryMoveRigidbody2D(const vec2& targetVelocity, float acceleration, float drag, float dt,
                               vec2* outVelocity = nullptr) {
    if (ScriptContext* scriptCtx = ctxPtr()) {
        return TryMoveRigidbody2D(*scriptCtx, targetVelocity, acceleration, drag, dt, outVelocity);
    }
    return false;
}

inline bool TryMoveRigidbody2D(const vec2& targetVelocity, float acceleration, float drag, float dt,
                               vec2& outVelocity) {
    if (ScriptContext* scriptCtx = ctxPtr()) {
        return TryMoveRigidbody2D(*scriptCtx, targetVelocity, acceleration, drag, dt, outVelocity);
    }
    return false;
}

inline void moveRigidbody2D(ScriptContext& ctx, const vec2& targetVelocity,
                            float acceleration, float drag, float dt) {
    (void)TryMoveRigidbody2D(ctx, targetVelocity, acceleration, drag, dt, nullptr);
}

inline void moveRigidbody2D(const vec2& targetVelocity, float acceleration, float drag, float dt) {
    if (ScriptContext* scriptCtx = ctxPtr()) {
        moveRigidbody2D(*scriptCtx, targetVelocity, acceleration, drag, dt);
    }
}

inline void movePosition2D(ScriptContext& ctx, const vec2& delta) {
    if (!ctx.object) return;
    vec2 pos = ctx.object->ui.position;
    pos += delta;
    ctx.SetPosition2D(pos);
}

inline void movePosition2D(const vec2& delta) {
    if (ScriptContext* scriptCtx = ctxPtr()) movePosition2D(*scriptCtx, delta);
}

inline bool warnOnce(ScriptContext& ctx, bool& alreadyWarned, const std::string& message,
                     ConsoleMessageType type = ConsoleMessageType::Warning) {
    if (alreadyWarned) return false;
    ctx.AddConsoleMessage(message, type);
    alreadyWarned = true;
    return true;
}

inline bool warnOnce(bool& alreadyWarned, const std::string& message,
                     ConsoleMessageType type = ConsoleMessageType::Warning) {
    if (ScriptContext* scriptCtx = ctxPtr()) return warnOnce(*scriptCtx, alreadyWarned, message, type);
    return false;
}

inline bool WarnOnce(bool& alreadyWarned, const std::string& message,
                     ConsoleMessageType type = ConsoleMessageType::Warning) {
    return warnOnce(alreadyWarned, message, type);
}

inline bool warnMissingComponentOnce(ScriptContext& ctx,
                                     bool& alreadyWarned,
                                     const char* scriptName,
                                     const char* componentName) {
    return warnOnce(ctx, alreadyWarned,
                    std::string(scriptName ? scriptName : "Script") +
                    ": missing " +
                    (componentName ? componentName : "required component") +
                    ".");
}

inline bool warnMissingComponentOnce(bool& alreadyWarned,
                                     const char* scriptName,
                                     const char* componentName) {
    if (ScriptContext* scriptCtx = ctxPtr()) {
        return warnMissingComponentOnce(*scriptCtx, alreadyWarned, scriptName, componentName);
    }
    return false;
}

inline bool hasAudioSource(ScriptContext& ctx) {
    return ctx.HasAudioSource();
}

inline bool hasAudioSource() {
    if (ScriptContext* scriptCtx = ctxPtr()) return hasAudioSource(*scriptCtx);
    return false;
}

inline bool playSound(ScriptContext& ctx, const std::string& clipPath = "", float volumeScale = 1.0f) {
    return ctx.PlayAudioOneShot(clipPath, volumeScale);
}

inline bool playSound(const std::string& clipPath = "", float volumeScale = 1.0f) {
    if (ScriptContext* scriptCtx = ctxPtr()) return playSound(*scriptCtx, clipPath, volumeScale);
    return false;
}

struct AudioFacade {
    bool HasSource() const { return hasAudioSource(); }
    bool PlayOneShot(const std::string& clipPath = "", float volumeScale = 1.0f) const {
        return playSound(clipPath, volumeScale);
    }
    bool Play() const {
        if (ScriptContext* scriptCtx = ctxPtr()) {
            return scriptCtx->PlayAudio();
        }
        return false;
    }
    bool Stop() const {
        if (ScriptContext* scriptCtx = ctxPtr()) {
            return scriptCtx->StopAudio();
        }
        return false;
    }
};

inline const AudioFacade audio{};

struct TimeFacade {
    float& deltaTime;
};

#if __cplusplus >= 201703L
inline thread_local TimeFacade time{ detail::gFrameDeltaTime };
#else
static thread_local TimeFacade time{ detail::gFrameDeltaTime };
#endif

struct EngineFacade {
    float& FPS;
};

#if __cplusplus >= 201703L
inline thread_local EngineFacade ModuEngine{ detail::gFrameFps };
#else
static thread_local EngineFacade ModuEngine{ detail::gFrameFps };
#endif

struct SpriteFacade {
    bool HasClips() const {
        if (ScriptContext* scriptCtx = ctxPtr()) {
            return scriptCtx->GetSpriteClipCount() > 0;
        }
        return false;
    }

    int ClipCount() const {
        if (ScriptContext* scriptCtx = ctxPtr()) {
            return scriptCtx->GetSpriteClipCount();
        }
        return 0;
    }

    int ClipIndex() const {
        if (ScriptContext* scriptCtx = ctxPtr()) {
            return scriptCtx->GetSpriteClipIndex();
        }
        return -1;
    }

    bool SetClip(int clipIndex) const {
        if (ScriptContext* scriptCtx = ctxPtr()) {
            return scriptCtx->SetSpriteClipIndex(clipIndex);
        }
        return false;
    }

    bool SetClip(const std::string& clipName) const {
        if (ScriptContext* scriptCtx = ctxPtr()) {
            return scriptCtx->SetSpriteClipName(clipName);
        }
        return false;
    }

    std::string ClipNameAt(int clipIndex) const {
        if (ScriptContext* scriptCtx = ctxPtr()) {
            return scriptCtx->GetSpriteClipNameAt(clipIndex);
        }
        return {};
    }
};

inline const SpriteFacade sprite{};

// the sheet-relative Sprite (ScriptSdkCommon.h) is THE Sprite type. keeps int<->Sprite
// conversion + IsAssigned() so clip-index containers and old call sites don't change.
using Sprite = ::Sprite;

struct Sprite8WaySet {
    std::array<Sprite, 8> directions{};

    Sprite& At(int direction) {
        return directions[std::clamp(direction, 0, 7)];
    }
    const Sprite& At(int direction) const {
        return directions[std::clamp(direction, 0, 7)];
    }
    int Clip(int direction) const {
        return At(direction).clipIndex;
    }
    void Set(int direction, int clipIndex) {
        At(direction).clipIndex = clipIndex;
    }

    template <typename Array8>
    static Sprite8WaySet FromIndexes(const Array8& clips) {
        Sprite8WaySet set;
        for (int i = 0; i < 8; ++i) set.directions[i] = clips[i];
        return set;
    }

    template <typename Array8>
    void WriteIndexes(Array8& clips) const {
        for (int i = 0; i < 8; ++i) clips[i] = directions[i].clipIndex;
    }
};

struct Sprite8WayFrames {
    std::array<std::array<Sprite, 4>, 8> directions{};

    Sprite& At(int direction, int frame) {
        return directions[std::clamp(direction, 0, 7)][std::clamp(frame, 0, 3)];
    }
    const Sprite& At(int direction, int frame) const {
        return directions[std::clamp(direction, 0, 7)][std::clamp(frame, 0, 3)];
    }
    int Clip(int direction, int frame) const {
        return At(direction, frame).clipIndex;
    }
    void Set(int direction, int frame, int clipIndex) {
        At(direction, frame).clipIndex = clipIndex;
    }

    template <typename Array8x4>
    static Sprite8WayFrames FromIndexes(const Array8x4& clips) {
        Sprite8WayFrames frames;
        for (int direction = 0; direction < 8; ++direction) {
            for (int frame = 0; frame < 4; ++frame) frames.directions[direction][frame] = clips[direction][frame];
        }
        return frames;
    }

    template <typename Array8x4>
    void WriteIndexes(Array8x4& clips) const {
        for (int direction = 0; direction < 8; ++direction) {
            for (int frame = 0; frame < 4; ++frame) clips[direction][frame] = directions[direction][frame].clipIndex;
        }
    }
};

struct Sprite4WayAnimation {
    std::array<std::vector<Sprite>, 4> directions{};

    std::vector<Sprite>& At(int direction) {
        return directions[std::clamp(direction, 0, 3)];
    }
    const std::vector<Sprite>& At(int direction) const {
        return directions[std::clamp(direction, 0, 3)];
    }
};

inline bool EditClipSelector(const char* label, int& clipIndex) {
    ScriptContext* scriptCtx = ctxPtr();
    if (!scriptCtx) return false;

    const int clipCount = scriptCtx->GetSpriteClipCount();
    std::string preview = detail::isValidClip(*scriptCtx, clipIndex)
        ? scriptCtx->GetSpriteClipNameAt(clipIndex)
        : std::string("<None>");

    bool changed = false;
    if (ModuGUI::BeginCombo(label, preview.c_str())) {
        const bool noneSelected = (clipIndex < 0 || clipIndex >= clipCount);
        if (ModuGUI::Selectable("<None>", noneSelected)) {
            clipIndex = -1;
            changed = true;
        }
        if (noneSelected) ModuGUI::SetItemDefaultFocus();

        for (int i = 0; i < clipCount; ++i) {
            const std::string clipName = scriptCtx->GetSpriteClipNameAt(i);
            const bool selected = (clipIndex == i);
            if (ModuGUI::Selectable(clipName.c_str(), selected)) {
                clipIndex = i;
                changed = true;
            }
            if (selected) ModuGUI::SetItemDefaultFocus();
        }

        ModuGUI::EndCombo();
    }

    return changed;
}

inline bool EditDirectionalClipGrid(std::array<int, 4>& idleClips,
                                    std::array<std::array<int, 4>, 4>& walkClips) {
    ScriptContext* scriptCtx = ctxPtr();
    if (!scriptCtx) return false;

    const int clipCount = scriptCtx->GetSpriteClipCount();
    if (clipCount <= 0) {
        ModuGUI::TextDisabled("Enable Sprite Sheet clips on this object to assign animation frames.");
        return false;
    }

    static constexpr const char* kDirectionLabels[4] = { "Down", "Up", "Right", "Left" };
    bool changed = false;
    ModuGUI::TextDisabled("%d sprite clips available.", clipCount);
    for (int dir = 0; dir < 4; ++dir) {
        if (ModuGUI::SubsectionFoldout(kDirectionLabels[dir], ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= EditClipSelector(("Idle##" + std::string(kDirectionLabels[dir])).c_str(), idleClips[dir]);
            for (int frame = 0; frame < 4; ++frame) {
                changed |= EditClipSelector(
                    ("Walk " + std::to_string(frame + 1) + "##" + std::string(kDirectionLabels[dir])).c_str(),
                    walkClips[dir][frame]);
            }
            ModuGUI::TreePop();
        }
    }
    return changed;
}

template <size_t N>
inline bool EditSoundSet(const char* heading, std::array<std::string, N>& sounds,
                         const char* itemPrefix = "Sound") {
    ModuGUI::TextUnformatted(heading);
    bool changed = false;

    for (size_t i = 0; i < N; ++i) {
        ModuGUI::PushID(static_cast<int>(i));

        std::vector<char> buffer(512, '\0');
        std::snprintf(buffer.data(), buffer.size(), "%s", sounds[i].c_str());
        ModuGUI::SetNextItemWidth(-84.0f);
        if (ModuGUI::InputText("##clip", buffer.data(), buffer.size())) {
            sounds[i] = buffer.data();
            changed = true;
        }

        if (ModuGUI::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ModuGUI::AcceptDragDropPayload("FILE_PATH")) {
                if (payload->Data && payload->DataSize > 0) {
                    const char* droppedPath = static_cast<const char*>(payload->Data);
                    if (droppedPath) {
                        std::string candidate = droppedPath;
                        if (detail::isAudioPath(candidate)) {
                            sounds[i] = candidate;
                            changed = true;
                        }
                    }
                }
            }
            ModuGUI::EndDragDropTarget();
        }

        ModuGUI::SameLine();
        if (ModuGUI::SmallButton("Clear")) {
            sounds[i].clear();
            changed = true;
        }

        ModuGUI::SameLine();
        ModuGUI::TextDisabled("%s %zu", itemPrefix, i + 1);

        ModuGUI::PopID();
    }

    return changed;
}

// beginner-friendly Scene / Movement / Ensure helpers. the `obj` inside Begin/Update/
// TickUpdate is the ObjectFacade injected by MODU_SCRIPT (see ModuCPPScriptApi.h).

namespace Scene {
inline SceneObj Find(const std::string& name) {
    ScriptContext* c = ctxPtr();
    return SceneObj{ c ? c->FindObjectByName(name) : nullptr };
}
inline SceneObj FindById(int id) {
    ScriptContext* c = ctxPtr();
    return SceneObj{ c ? c->FindObjectById(id) : nullptr };
}
inline bool Exists(const std::string& name) {
    ScriptContext* c = ctxPtr();
    return c && c->FindObjectByName(name) != nullptr;
}
} // namespace Scene

namespace Movement {

inline Vector3 Direction(const Vector2& move, const SceneObject* reference) {
    const float mag = glm::length(move);
    if (mag < 1e-4f) return Vector3(0.0f);
    Vector2 normalized = (mag > 1.0f) ? (move / mag) : move;

    Vector3 forward(0.0f, 0.0f, -1.0f);
    Vector3 right(1.0f, 0.0f, 0.0f);
    if (reference) {
        const glm::quat q = glm::quat(glm::radians(reference->rotation));
        Vector3 f = q * Vector3(0.0f, 0.0f, -1.0f);
        Vector3 r = q * Vector3(1.0f, 0.0f, 0.0f);
        f.y = 0.0f;
        r.y = 0.0f;
        const float fl = glm::length(f);
        const float rl = glm::length(r);
        if (fl > 1e-3f) forward = f / fl;
        if (rl > 1e-3f) right   = r / rl;
    }

    Vector3 dir = right * normalized.x + forward * normalized.y;
    const float len = glm::length(dir);
    if (len > 1e-4f) dir /= len;
    return dir;
}

inline Vector3 Direction(const Vector2& move, const SceneObj& reference) {
    return Direction(move, static_cast<const SceneObject*>(reference.ptr));
}

inline Vector3 Direction(const Vector2& move, const ObjectFacade& reference) {
    return Direction(move, reference.raw());
}

// Public-field SceneObj declarations are stored as `std::string` (the object
// name reference) by the ModuCPP transpiler. Resolve at call time.
inline Vector3 Direction(const Vector2& move, const std::string& referenceName) {
    ScriptContext* c = ctxPtr();
    const SceneObject* ref = (c && !referenceName.empty()) ? c->FindObjectByName(referenceName) : nullptr;
    return Direction(move, ref);
}

inline Vector3 Direction(const Vector2& move) {
    return Direction(move, static_cast<const SceneObject*>(nullptr));
}

} // namespace Movement

namespace Ensure {

namespace detail {
inline bool applyRigidbodyEnsure(SceneObject* target, bool isCurrentCtx,
                                 bool freezeRotation, bool useGravity) {
    ScriptContext* c = ctxPtr();
    if (!c || !target) return false;
    if (isCurrentCtx) {
        const bool ok = c->EnsureRigidbody(useGravity, /*kinematic=*/false);
        if (ok && freezeRotation && c->object) {
            c->object->rigidbody.lockRotationX = true;
            c->object->rigidbody.lockRotationY = true;
            c->object->rigidbody.lockRotationZ = true;
            c->MarkDirty();
        }
        return ok;
    }
    if (!target->hasRigidbody) {
        target->hasRigidbody = true;
        target->rigidbody = RigidbodyComponent{};
    }
    target->rigidbody.useGravity = useGravity;
    if (freezeRotation) {
        target->rigidbody.lockRotationX = true;
        target->rigidbody.lockRotationY = true;
        target->rigidbody.lockRotationZ = true;
    }
    c->MarkDirty();
    return true;
}
} // namespace detail

// visual sentinel: `Ensure::obj;` is a no-op marker that documents intent. the real check
// happens inside any ScriptContext call that needs an object.
inline constexpr int obj = 0;

inline bool Rigidbody3D(const SceneObj& target, bool freezeRotation = false, bool useGravity = true) {
    const bool currentCtx = (ctxPtr() && target.ptr == ctxPtr()->object);
    return detail::applyRigidbodyEnsure(target.ptr, currentCtx, freezeRotation, useGravity);
}

inline bool Rigidbody3D(const ObjectFacade&, bool freezeRotation = false, bool useGravity = true) {
    return detail::applyRigidbodyEnsure(ctxPtr() ? ctxPtr()->object : nullptr,
                                        /*isCurrentCtx=*/true, freezeRotation, useGravity);
}

inline bool Rigidbody3D(bool freezeRotation = false, bool useGravity = true) {
    return detail::applyRigidbodyEnsure(ctxPtr() ? ctxPtr()->object : nullptr,
                                        /*isCurrentCtx=*/true, freezeRotation, useGravity);
}

inline bool CapsuleCollider(float height = 1.8f, float radius = 0.4f) {
    if (ScriptContext* c = ctxPtr()) return c->EnsureCapsuleCollider(height, radius);
    return false;
}

} // namespace Ensure

} // namespace ModuCPP
