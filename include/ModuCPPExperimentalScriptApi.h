#pragma once

#include "ModuCPPScriptApi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <regex>
#include <unordered_map>

namespace ModuCPP {

inline std::string Trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

inline int ParseInt(const std::string& value, int fallback = 0) {
    if (value.empty()) return fallback;
    char* endPtr = nullptr;
    long parsed = std::strtol(value.c_str(), &endPtr, 10);
    if (endPtr == value.c_str()) return fallback;
    if (parsed < std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
    if (parsed > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return static_cast<int>(parsed);
}

inline float ParseFloat(const std::string& value, float fallback = 0.0f) {
    if (value.empty()) return fallback;
    char* endPtr = nullptr;
    float parsed = std::strtof(value.c_str(), &endPtr);
    if (endPtr == value.c_str()) return fallback;
    return parsed;
}

inline bool ParseBool(const std::string& value, bool fallback = false) {
    std::string lowered = Trim(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") return true;
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") return false;
    return fallback;
}

inline std::string GetScriptSetting(const ScriptComponent* script, const std::string& key,
                                    const std::string& fallback = "") {
    if (!script) return fallback;
    for (const ScriptSetting& setting : script->settings) {
        if (setting.key == key) {
            return setting.value;
        }
    }
    return fallback;
}

inline bool SetScriptSetting(ScriptComponent* script, const std::string& key, const std::string& value) {
    if (!script) return false;
    for (ScriptSetting& setting : script->settings) {
        if (setting.key == key) {
            if (setting.value == value) return false;
            setting.value = value;
            return true;
        }
    }
    script->settings.push_back(ScriptSetting{key, value});
    return true;
}

inline std::string EscapeField(const std::string& value, char delimiter) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '\\' || c == delimiter || c == '\n' || c == '\r') {
            out.push_back('\\');
            if (c == '\n') out.push_back('n');
            else if (c == '\r') out.push_back('r');
            else out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

inline std::string UnescapeField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    bool escaped = false;
    for (char c : value) {
        if (escaped) {
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else out.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        out.push_back(c);
    }
    if (escaped) out.push_back('\\');
    return out;
}

inline std::vector<std::string> SplitEscaped(const std::string& value, char delimiter) {
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (char c : value) {
        if (escaped) {
            if (c == 'n') current.push_back('\n');
            else if (c == 'r') current.push_back('\r');
            else current.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == delimiter) {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (escaped) current.push_back('\\');
    fields.push_back(current);
    return fields;
}

inline std::string JoinEscaped(const std::vector<std::string>& values, char delimiter) {
    std::string joined;
    for (size_t i = 0; i < values.size(); ++i) {
        joined += EscapeField(values[i], delimiter);
        if (i + 1 < values.size()) joined.push_back(delimiter);
    }
    return joined;
}

inline std::vector<std::string> DeserializeObjectRefs(const std::string& encoded) {
    std::vector<std::string> refs;
    if (encoded.empty()) return refs;
    std::vector<std::string> parsed = SplitEscaped(encoded, ';');
    refs.reserve(parsed.size());
    for (const std::string& item : parsed) {
        std::string trimmed = Trim(item);
        if (!trimmed.empty()) refs.push_back(trimmed);
    }
    return refs;
}

inline std::string SerializeObjectRefs(const std::vector<std::string>& refs) {
    std::vector<std::string> cleaned;
    cleaned.reserve(refs.size());
    for (const std::string& ref : refs) {
        std::string trimmed = Trim(ref);
        if (!trimmed.empty()) cleaned.push_back(trimmed);
    }
    return JoinEscaped(cleaned, ';');
}

inline std::string MakeObjectRef(int objectId) {
    return std::string("Object.ID-") + std::to_string(objectId);
}

inline bool IsAllDigits(const std::string& value) {
    if (value.empty()) return false;
    size_t start = (value[0] == '-' || value[0] == '+') ? 1 : 0;
    if (start >= value.size()) return false;
    for (size_t i = start; i < value.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

inline SceneObject* ResolveSceneObjectRef(ScriptContext& ctx, const std::string& objectRef) {
    std::string trimmed = Trim(objectRef);
    if (trimmed.empty()) return nullptr;

    if (SceneObject* resolved = ctx.ResolveObjectRef(trimmed)) {
        return resolved;
    }

    if (IsAllDigits(trimmed)) {
        return ctx.FindObjectById(ParseInt(trimmed, -1));
    }

    return ctx.FindObjectByName(trimmed);
}

inline bool SetObjectEnabledState(ScriptContext& ctx, SceneObject* object, bool enabled) {
    if (!object) return false;
    if (object->enabled == enabled) return false;
    object->enabled = enabled;
    ctx.MarkDirty();
    return true;
}

inline bool SetObjectsEnabledState(ScriptContext& ctx, const std::vector<std::string>& refs, bool enabled) {
    bool changed = false;
    for (const std::string& ref : refs) {
        SceneObject* object = ResolveSceneObjectRef(ctx, ref);
        if (SetObjectEnabledState(ctx, object, enabled)) {
            changed = true;
        }
    }
    return changed;
}

inline glm::vec3 GetObjectReferencePosition(ScriptContext& ctx, const SceneObject& object) {
    if (HasUIComponent(object) && object.ui.type != UIElementType::None) {
        glm::vec2 worldUiPos(object.ui.position.x, object.ui.position.y);
        int parentId = object.parentId;
        int guard = 0;
        while (parentId >= 0 && guard < 256) {
            ++guard;
            SceneObject* parent = ctx.FindObjectById(parentId);
            if (!parent) break;
            if (HasUIComponent(*parent) && parent->ui.type != UIElementType::None) {
                worldUiPos += parent->ui.position;
                parentId = parent->parentId;
                continue;
            }
            worldUiPos += glm::vec2(parent->position.x, parent->position.y);
            break;
        }
        return glm::vec3(worldUiPos.x, worldUiPos.y, object.position.z);
    }
    return object.position;
}

inline std::string GetCurrentObjectName(ScriptContext& ctx, const std::string& fallback = "Object") {
    if (ctx.object && !ctx.object->name.empty()) {
        return ctx.object->name;
    }
    return fallback;
}

inline bool TryPlayAnimationClipNamed(ScriptContext& ctx, const std::string& clipName) {
    if (!ctx.object || !ctx.object->hasAnimation || !ctx.object->animation.enabled) return false;
    SceneObject& object = *ctx.object;
    NormalizeAnimationClipSlots(object.animation);
    const std::string needle = Trim(clipName);
    int clipIndex = -1;
    for (int i = 0; i < static_cast<int>(object.animation.clips.size()); ++i) {
        const AnimationClipSlot& clip = object.animation.clips[static_cast<size_t>(i)];
        std::string clipDisplayName = clip.name.empty() ? AnimationClipNameFromPath(clip.assetPath) : clip.name;
        if (clipDisplayName == needle) {
            clipIndex = i;
            break;
        }
    }
    if (clipIndex < 0) return false;

    object.animation.activeClipIndex = clipIndex;
    object.animation.clipAssetPath = object.animation.clips[static_cast<size_t>(clipIndex)].assetPath;
    if (object.animation.clipAssetPath.empty()) return false;
    object.animation.runtimeClipPath = object.animation.clipAssetPath;
    object.animation.runtimeTime = 0.0f;
    object.animation.runtimePlaying = true;
    object.animation.runtimePaused = false;
    object.animation.runtimeDirection = 1.0f;
    object.animation.runtimeInitialized = true;
    return true;
}

inline SceneObject* ResolveUITextTarget(ScriptContext& ctx, const std::string& objectRef) {
    SceneObject* object = ResolveSceneObjectRef(ctx, objectRef);
    if (!object) return nullptr;
    if (HasUIComponent(*object) && object->ui.type == UIElementType::Text) {
        return object;
    }

    std::vector<int> pendingChildIds = object->childIds;
    int childGuard = 0;
    while (!pendingChildIds.empty() && childGuard < 512) {
        const int childId = pendingChildIds.back();
        pendingChildIds.pop_back();
        ++childGuard;
        SceneObject* child = ctx.FindObjectById(childId);
        if (!child) continue;
        if (HasUIComponent(*child) && child->ui.type == UIElementType::Text) {
            return child;
        }
        pendingChildIds.insert(pendingChildIds.end(), child->childIds.begin(), child->childIds.end());
    }

    int parentId = object->parentId;
    int parentGuard = 0;
    while (parentId >= 0 && parentGuard < 256) {
        ++parentGuard;
        SceneObject* parent = ctx.FindObjectById(parentId);
        if (!parent) break;
        if (HasUIComponent(*parent) && parent->ui.type == UIElementType::Text) {
            return parent;
        }
        parentId = parent->parentId;
    }

    return nullptr;
}

inline bool SetUITextLabel(ScriptContext& ctx, const std::string& objectRef, const std::string& label) {
    SceneObject* object = ResolveUITextTarget(ctx, objectRef);
    if (!object) return false;
    if (object->ui.label == label) return false;
    object->ui.label = label;
    ctx.MarkDirty();
    return true;
}

inline bool SetUITextEffects(ScriptContext& ctx,
                             const std::string& objectRef,
                             int effectFlags,
                             float animationSpeed,
                             float effectIntensity) {
    SceneObject* object = ResolveUITextTarget(ctx, objectRef);
    if (!object) return false;

    const float speed = std::max(0.01f, animationSpeed);
    const float intensity = std::max(0.0f, effectIntensity);

    bool changed = false;
    if (object->ui.textEffectFlags != effectFlags) {
        object->ui.textEffectFlags = effectFlags;
        changed = true;
    }
    if (std::abs(object->ui.textEffectSpeed - speed) > 1e-5f) {
        object->ui.textEffectSpeed = speed;
        changed = true;
    }
    if (std::abs(object->ui.textEffectIntensity - intensity) > 1e-5f) {
        object->ui.textEffectIntensity = intensity;
        changed = true;
    }

    if (changed) {
        ctx.MarkDirty();
    }
    return changed;
}

// Typewriter reveal for a UI Text element, matched to SetUITextLabel above.
//
// Set the label ONCE to the whole message, then walk revealChars up from 0. The
// renderer measures wrapping and alignment from the full string and simply skips
// glyphs past the head, so a centred line does not crawl sideways as it types -
// which is what happens if you feed it label.substr(0, n) instead.
//
// popScale is the size a glyph is born at (1.0 = no pop), softness is how many
// glyphs it takes to settle back to normal while fading in.
// Pass revealChars < 0 to switch the reveal off and draw the label in full.
inline bool SetUITextReveal(ScriptContext& ctx,
                            const std::string& objectRef,
                            float revealChars,
                            float popScale = 1.0f,
                            float softness = 1.0f) {
    SceneObject* object = ResolveUITextTarget(ctx, objectRef);
    if (!object) return false;

    const float safePop = std::max(0.01f, popScale);
    const float safeSoft = std::max(0.0001f, softness);

    bool changed = false;
    if (std::abs(object->ui.textRevealChars - revealChars) > 1e-5f) {
        object->ui.textRevealChars = revealChars;
        changed = true;
    }
    if (std::abs(object->ui.textRevealPopScale - safePop) > 1e-5f) {
        object->ui.textRevealPopScale = safePop;
        changed = true;
    }
    if (std::abs(object->ui.textRevealSoftness - safeSoft) > 1e-5f) {
        object->ui.textRevealSoftness = safeSoft;
        changed = true;
    }
    // Deliberately no MarkDirty: the reveal head moves every frame and is not
    // authored data, so flagging the project unsaved on it would be noise.
    return changed;
}

// Text colour alpha, by the same objectRef the label helpers take. Kept next to
// them so a script can fade a line out without having to resolve a SceneObj and
// reach through obj.UI - which only works on declared [ObjectRef] fields.
inline float GetUITextAlpha(ScriptContext& ctx, const std::string& objectRef) {
    SceneObject* object = ResolveUITextTarget(ctx, objectRef);
    if (!object) return 1.0f;
    return object->ui.color.a;
}

inline bool SetUITextAlpha(ScriptContext& ctx, const std::string& objectRef, float alpha) {
    SceneObject* object = ResolveUITextTarget(ctx, objectRef);
    if (!object) return false;
    const float clamped = std::clamp(alpha, 0.0f, 1.0f);
    if (std::abs(object->ui.color.a - clamped) <= 1e-5f) return false;
    object->ui.color.a = clamped;
    return true;
}

// Number of glyphs (not bytes) in a UTF-8 string - the unit SetUITextReveal counts in.
inline int UITextGlyphCount(const std::string& text) {
    int count = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t step = 1;
        if ((c & 0x80u) != 0) {
            if ((c & 0xE0u) == 0xC0u) step = 2;
            else if ((c & 0xF0u) == 0xE0u) step = 3;
            else if ((c & 0xF8u) == 0xF0u) step = 4;
        }
        i += step;
        ++count;
    }
    return count;
}

inline bool SetRigidbody2DSimulated(ScriptContext& ctx, const std::string& objectRef, bool simulated) {
    SceneObject* object = ResolveSceneObjectRef(ctx, objectRef);
    if (!object || !object->hasRigidbody2D) return false;
    if (object->rigidbody2D.enabled == simulated) return false;
    object->rigidbody2D.enabled = simulated;
    ctx.MarkDirty();
    return true;
}

inline bool DrawStdStringInput(const char* label, std::string& value,
                               size_t capacity = 512,
                               ImGuiInputTextFlags flags = 0,
                               bool multiline = false,
                               float multilineHeight = 90.0f) {
    std::vector<char> buffer(capacity + 1, '\0');
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
    bool edited = false;
    if (multiline) {
        edited = ModuGUI::InputTextMultiline(label, buffer.data(), buffer.size(),
                                           ImVec2(-FLT_MIN, multilineHeight), flags);
    } else {
        edited = ModuGUI::InputText(label, buffer.data(), buffer.size(), flags);
    }
    if (edited) {
        value = buffer.data();
    }
    return edited;
}

inline bool IsAudioClipPath(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == "wav" || ext == "mp3" || ext == "ogg" || ext == "flac" ||
           ext == "aac" || ext == "m4a";
}

inline bool DrawAudioClipInput(const char* label, std::string& clipPath, size_t capacity = 512) {
    bool changed = DrawStdStringInput(label, clipPath, capacity);
    if (ModuGUI::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ModuGUI::AcceptDragDropPayload("FILE_PATH")) {
            if (payload->Data && payload->DataSize > 0) {
                const char* droppedPath = static_cast<const char*>(payload->Data);
                if (droppedPath) {
                    std::string candidate = droppedPath;
                    if (IsAudioClipPath(candidate) && clipPath != candidate) {
                        clipPath = std::move(candidate);
                        changed = true;
                    }
                }
            }
        }
        ModuGUI::EndDragDropTarget();
    }
    return changed;
}

inline bool DrawObjectRefListEditor(ScriptContext& ctx, const char* label, std::vector<std::string>& refs) {
    bool changed = false;
    if (!ModuGUI::TreeNode(label)) {
        return false;
    }

    for (size_t i = 0; i < refs.size(); ++i) {
        ModuGUI::PushID(static_cast<int>(i));
        const std::string rowLabel = std::string("Reference ") + std::to_string(i + 1);
        changed |= ::ModuCPP::DrawObjectRefInput(ctx, rowLabel.c_str(), refs[i]);
        if (ModuGUI::SmallButton("Remove")) {
            refs.erase(refs.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            ModuGUI::PopID();
            --i;
            continue;
        }
        ModuGUI::PopID();
    }

    if (ModuGUI::Button("Add Reference")) {
        refs.emplace_back();
        changed = true;
    }
    ModuGUI::SameLine();
    if (ModuGUI::Button("Add Selected")) {
        const int selectedId = ctx.GetSelectedObjectId();
        if (selectedId >= 0) {
            refs.push_back(MakeObjectRef(selectedId));
            changed = true;
        }
    }

    if (ModuGUI::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ModuGUI::AcceptDragDropPayload("SCENE_OBJECT")) {
            if (payload->Data && payload->DataSize == static_cast<int>(sizeof(int))) {
                const int droppedId = *static_cast<const int*>(payload->Data);
                refs.push_back(MakeObjectRef(droppedId));
                changed = true;
            }
        }
        ModuGUI::EndDragDropTarget();
    }

    ModuGUI::TreePop();
    return changed;
}

} // namespace ModuCPP
