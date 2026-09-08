#pragma once
#include "SceneObject.h"
#include <cctype>
#include <cstdio>
namespace AnimationBinding {
    inline bool IsLocalTransformProperty(const std::string& propertyId) {
        return propertyId.rfind("localPosition.", 0) == 0 || propertyId.rfind("localRotation.", 0) == 0 || propertyId.rfind("localScale.", 0) == 0;
    }
    inline std::string EncodeToken(const std::string& value) {
        std::string out;
        out.reserve(value.size() + 8);
        for (char c : value) {
            if (c == '%' || c == '.' || c == '/' || c == ':') {
                static const char* kHex = "0123456789ABCDEF";
                out.push_back('%');
                out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0x0F]);
                out.push_back(kHex[static_cast<unsigned char>(c) & 0x0F]);
            }
            else {out.push_back(c);}
        }   return out;
    }
    inline bool DecodeHexNibble(char c, unsigned char& out) {
        if (c >= '0' && c <= '9') { out = static_cast<unsigned char>(c - '0'); return true; }
        if (c >= 'A' && c <= 'F') { out = static_cast<unsigned char>(10 + (c - 'A')); return true; }
        if (c >= 'a' && c <= 'f') { out = static_cast<unsigned char>(10 + (c - 'a')); return true; }
        return false;
    }
    inline std::string DecodeToken(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                unsigned char hi = 0;
                unsigned char lo = 0;
                if (DecodeHexNibble(value[i + 1], hi) && DecodeHexNibble(value[i + 2], lo)) {
                    out.push_back(static_cast<char>((hi << 4) | lo)); i += 2; continue;
                }
            }
            out.push_back(value[i]);
        }   return out;
    }
    inline bool TryParseFloatValue(const std::string& text, float& outValue) {
        const char* begin = text.c_str();
        char* end = nullptr;
        outValue = std::strtof(begin, &end);
        if (end == begin) {
            std::string lower = text;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (lower == "true" || lower == "yes" || lower == "on")  {outValue = 1.0f; return true;}
            if (lower == "false" || lower == "no" || lower == "off") {outValue = 0.0f; return true;}
            return false;
        }
        while (*end != '\0') {
            if (!std::isspace(static_cast<unsigned char>(*end))) return false; ++end;
        }   return true;
    }
    inline std::string MakeScriptSettingPropertyId(int scriptInspectorId, const std::string& settingKey) {
        return "ScriptSetting." + std::to_string(scriptInspectorId) + ".key." + EncodeToken(settingKey);
    }
    inline bool ParseScriptSettingPropertyId(const std::string& propertyId, int& outScriptId, bool& outUsesLegacyIndex, int& outLegacyIndex, std::string& outSettingKey) {
        const std::string prefix = "ScriptSetting.";
        if (propertyId.rfind(prefix, 0) != 0) return false;
        const size_t scriptSep = propertyId.find('.', prefix.size());
        if (scriptSep == std::string::npos) return false;
        const std::string scriptPart = propertyId.substr(prefix.size(), scriptSep - prefix.size());
        const std::string settingPart = propertyId.substr(scriptSep + 1);
        try {outScriptId = std::stoi(scriptPart);}
        catch (...) {return false;}
        outUsesLegacyIndex = false;
        outLegacyIndex = -1;
        outSettingKey.clear();
        const std::string keyPrefix = "key.";
        if (settingPart.rfind(keyPrefix, 0) == 0) {
            outSettingKey = DecodeToken(settingPart.substr(keyPrefix.size())); return true;
        }
        if (!settingPart.empty() &&
            std::all_of(settingPart.begin(), settingPart.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            outUsesLegacyIndex = true;
            try {outLegacyIndex = std::stoi(settingPart);}
            catch (...) {return false;}
            return true;
        }
        outSettingKey = DecodeToken(settingPart);
        return true;
    }
    inline const ScriptComponent* FindScriptByInspectorId(const SceneObject& obj, int inspectorId) {
        for (const ScriptComponent& script : obj.scripts) {if (script.inspectorId == inspectorId) return &script;} return nullptr;
    }
    inline ScriptComponent* FindScriptByInspectorId(SceneObject& obj, int inspectorId) {
        for (ScriptComponent& script : obj.scripts) {if (script.inspectorId == inspectorId) return &script;} return nullptr;
    }
    inline std::string PrettyPropertyName(const std::string& propertyId) {
        int scriptId = -1;
        bool legacy = false;
        int legacyIndex = -1;
        std::string settingKey;
        if (ParseScriptSettingPropertyId(propertyId, scriptId, legacy, legacyIndex, settingKey)) {
            std::string label = "Script Setting / " + std::to_string(scriptId) + " / ";
            if (legacy) {label += "#" + std::to_string(legacyIndex);}
            else {label += settingKey.empty() ? "<unnamed>" : settingKey;}
            return label;
        }
        std::string label;
        label.reserve(propertyId.size() + 8);
        for (char c : propertyId) {
            if (c == '.') {label += " / ";}
            else {label.push_back(c);}
        }   return label;
    }
    inline bool ReadProperty(const SceneObject& obj, const std::string& propertyId, float& outValue) {
        if (propertyId == "localPosition.x") { outValue = obj.localPosition.x; return true; }
        if (propertyId == "localPosition.y") { outValue = obj.localPosition.y; return true; }
        if (propertyId == "localPosition.z") { outValue = obj.localPosition.z; return true; }
        if (propertyId == "localRotation.x") { outValue = obj.localRotation.x; return true; }
        if (propertyId == "localRotation.y") { outValue = obj.localRotation.y; return true; }
        if (propertyId == "localRotation.z") { outValue = obj.localRotation.z; return true; }
        if (propertyId == "localScale.x") { outValue = obj.localScale.x; return true; }
        if (propertyId == "localScale.y") { outValue = obj.localScale.y; return true; }
        if (propertyId == "localScale.z") { outValue = obj.localScale.z; return true; }
        if (propertyId == "UI.Position.x" && obj.hasUI) { outValue = obj.ui.position.x; return true; }
        if (propertyId == "UI.Position.y" && obj.hasUI) { outValue = obj.ui.position.y; return true; }
        if (propertyId == "UI.Size.x" && obj.hasUI) { outValue = obj.ui.size.x; return true; }
        if (propertyId == "UI.Size.y" && obj.hasUI) { outValue = obj.ui.size.y; return true; }
        if (propertyId == "UI.Rotation" && obj.hasUI) { outValue = obj.ui.rotation; return true; }
        if (propertyId == "UI.SliderValue" && obj.hasUI) { outValue = obj.ui.sliderValue; return true; }
        if (propertyId == "UI.TextScale" && obj.hasUI) { outValue = obj.ui.textScale; return true; }
        if (propertyId == "UI.Color.r" && obj.hasUI) { outValue = obj.ui.color.r; return true; }
        if (propertyId == "UI.Color.g" && obj.hasUI) { outValue = obj.ui.color.g; return true; }
        if (propertyId == "UI.Color.b" && obj.hasUI) { outValue = obj.ui.color.b; return true; }
        if (propertyId == "UI.Color.a" && obj.hasUI) { outValue = obj.ui.color.a; return true; }
        if (propertyId == "UI.Interactable" && obj.hasUI) { outValue = obj.ui.interactable ? 1.0f : 0.0f; return true; }
        if (propertyId == "UI.Anchor" && obj.hasUI) { outValue = static_cast<float>(obj.ui.anchor); return true; }
        if (propertyId == "UI.RenderIn3D" && obj.hasUI) { outValue = obj.ui.renderIn3D ? 1.0f : 0.0f; return true; }
        if (propertyId == "UI.SpriteSheetEnabled" && obj.hasUI) { outValue = obj.ui.spriteSheetEnabled ? 1.0f : 0.0f; return true; }
        if (propertyId == "UI.SpriteCustomFramesEnabled" && obj.hasUI) { outValue = obj.ui.spriteCustomFramesEnabled ? 1.0f : 0.0f; return true; }
        if (propertyId == "UI.MaskChildren" && obj.hasUI) { outValue = obj.ui.maskChildren ? 1.0f : 0.0f; return true; }
        if (propertyId == "UI.SpriteFrame" && obj.hasUI) { outValue = static_cast<float>(obj.ui.spriteSheetFrame); return true; }
        if (propertyId == "UI.SpriteSheetFPS" && obj.hasUI) { outValue = obj.ui.spriteSheetFps; return true; }
        if (propertyId == "UI.SpriteSheetLoop" && obj.hasUI) { outValue = obj.ui.spriteSheetLoop ? 1.0f : 0.0f; return true; }
        if (propertyId == "UI.TextEffectFlags" && obj.hasUI) { outValue = static_cast<float>(obj.ui.textEffectFlags); return true; }
        if (propertyId == "UI.TextEffectSpeed" && obj.hasUI) { outValue = obj.ui.textEffectSpeed; return true; }
        if (propertyId == "UI.TextEffectIntensity" && obj.hasUI) { outValue = obj.ui.textEffectIntensity; return true; }
        if (propertyId == "UI.ReceiveLighting2D" && obj.hasUI) { outValue = obj.ui.receiveLighting2D ? 1.0f : 0.0f; return true; }
        if (propertyId == "UI.UnlitLighting2D" && obj.hasUI) { outValue = obj.ui.unlitLighting2D ? 1.0f : 0.0f; return true; }
        if (propertyId == "UI.EmissiveLighting2D" && obj.hasUI) { outValue = obj.ui.emissiveLighting2D; return true; }
        if (propertyId == "Light.Color.r" && obj.hasLight) { outValue = obj.light.color.r; return true; }
        if (propertyId == "Light.Color.g" && obj.hasLight) { outValue = obj.light.color.g; return true; }
        if (propertyId == "Light.Color.b" && obj.hasLight) { outValue = obj.light.color.b; return true; }
        if (propertyId == "Light.Intensity" && obj.hasLight) { outValue = obj.light.intensity; return true; }
        if (propertyId == "Light.Range" && obj.hasLight) { outValue = obj.light.range; return true; }
        if (propertyId == "Light.InnerAngle" && obj.hasLight) { outValue = obj.light.innerAngle; return true; }
        if (propertyId == "Light.OuterAngle" && obj.hasLight) { outValue = obj.light.outerAngle; return true; }
        if (propertyId == "Light.Enabled" && obj.hasLight) { outValue = obj.light.enabled ? 1.0f : 0.0f; return true; }
        if (propertyId == "Camera.FOV" && obj.hasCamera) { outValue = obj.camera.fov; return true; }
        if (propertyId == "Camera.NearClip" && obj.hasCamera) { outValue = obj.camera.nearClip; return true; }
        if (propertyId == "Camera.FarClip" && obj.hasCamera) { outValue = obj.camera.farClip; return true; }
        if (propertyId == "Camera.PixelsPerUnit" && obj.hasCamera) { outValue = obj.camera.pixelsPerUnit; return true; }
        if (propertyId == "Camera.ApplyPostFX" && obj.hasCamera) { outValue = obj.camera.applyPostFX ? 1.0f : 0.0f; return true; }
        if (propertyId == "Camera.Use2D" && obj.hasCamera) { outValue = obj.camera.use2D ? 1.0f : 0.0f; return true; }
        if (propertyId == "PostFX.BloomIntensity" && obj.hasPostFX) { outValue = obj.postFx.bloomIntensity; return true; }
        if (propertyId == "PostFX.Exposure" && obj.hasPostFX) { outValue = obj.postFx.exposure; return true; }
        if (propertyId == "PostFX.Contrast" && obj.hasPostFX) { outValue = obj.postFx.contrast; return true; }
        if (propertyId == "PostFX.Saturation" && obj.hasPostFX) { outValue = obj.postFx.saturation; return true; }
        if (propertyId == "PostFX.ColorFilter.r" && obj.hasPostFX) { outValue = obj.postFx.colorFilter.r; return true; }
        if (propertyId == "PostFX.ColorFilter.g" && obj.hasPostFX) { outValue = obj.postFx.colorFilter.g; return true; }
        if (propertyId == "PostFX.ColorFilter.b" && obj.hasPostFX) { outValue = obj.postFx.colorFilter.b; return true; }
        if (propertyId == "PostFX.Enabled" && obj.hasPostFX) { outValue = obj.postFx.enabled ? 1.0f : 0.0f; return true; }
        if (propertyId == "Material.Color.r") { outValue = obj.material.color.r; return true; }
        if (propertyId == "Material.Color.g") { outValue = obj.material.color.g; return true; }
        if (propertyId == "Material.Color.b") { outValue = obj.material.color.b; return true; }
        if (propertyId == "Material.Alpha") { outValue = obj.material.alpha; return true; }
        if (propertyId == "Material.AmbientStrength") { outValue = obj.material.ambientStrength; return true; }
        if (propertyId == "Material.SpecularStrength") { outValue = obj.material.specularStrength; return true; }
        if (propertyId == "Material.Shininess") { outValue = obj.material.shininess; return true; }
        if (propertyId == "Material.NormalMapIntensity") { outValue = obj.material.normalMapIntensity; return true; }
        if (propertyId == "Material.TextureMix") { outValue = obj.material.textureMix; return true; }
        if (propertyId == "Rigidbody.Mass" && obj.hasRigidbody) { outValue = obj.rigidbody.mass; return true; }
        if (propertyId == "Rigidbody.UseGravity" && obj.hasRigidbody) { outValue = obj.rigidbody.useGravity ? 1.0f : 0.0f; return true; }
        if (propertyId == "Rigidbody.IsKinematic" && obj.hasRigidbody) { outValue = obj.rigidbody.isKinematic ? 1.0f : 0.0f; return true; }
        if (propertyId == "Rigidbody.LinearDamping" && obj.hasRigidbody) { outValue = obj.rigidbody.linearDamping; return true; }
        if (propertyId == "Rigidbody.AngularDamping" && obj.hasRigidbody) { outValue = obj.rigidbody.angularDamping; return true; }
        if (propertyId == "Rigidbody.LockRotationX" && obj.hasRigidbody) { outValue = obj.rigidbody.lockRotationX ? 1.0f : 0.0f; return true; }
        if (propertyId == "Rigidbody.LockRotationY" && obj.hasRigidbody) { outValue = obj.rigidbody.lockRotationY ? 1.0f : 0.0f; return true; }
        if (propertyId == "Rigidbody.LockRotationZ" && obj.hasRigidbody) { outValue = obj.rigidbody.lockRotationZ ? 1.0f : 0.0f; return true; }
        if (propertyId == "Audio.Volume" && obj.hasAudioSource) { outValue = obj.audioSource.volume; return true; }
        if (propertyId == "Audio.MinDistance" && obj.hasAudioSource) { outValue = obj.audioSource.minDistance; return true; }
        if (propertyId == "Audio.MaxDistance" && obj.hasAudioSource) { outValue = obj.audioSource.maxDistance; return true; }
        if (propertyId == "Audio.Loop" && obj.hasAudioSource) { outValue = obj.audioSource.loop ? 1.0f : 0.0f; return true; }
        if (propertyId == "Audio.Spatial" && obj.hasAudioSource) { outValue = AudioSourceUsesSpatialization(obj.audioSource) ? 1.0f : 0.0f; return true; }
        if (propertyId == "AIAgent.Speed" && obj.hasAIAgent) { outValue = obj.aiAgent.speed; return true; }
        if (propertyId == "AIAgent.StoppingDistance" && obj.hasAIAgent) { outValue = obj.aiAgent.stoppingDistance; return true; }
        int scriptId = -1;
        bool legacyIndex = false;
        int settingIndex = -1;
        std::string settingKey;
        if (ParseScriptSettingPropertyId(propertyId, scriptId, legacyIndex, settingIndex, settingKey)) {
            const ScriptComponent* script = FindScriptByInspectorId(obj, scriptId);
            if (!script && scriptId >= 0 && scriptId < static_cast<int>(obj.scripts.size())) {script = &obj.scripts[static_cast<size_t>(scriptId)];}
            if (script) {
                if (legacyIndex) {
                    if (settingIndex >= 0 && settingIndex < static_cast<int>(script->settings.size())) {return TryParseFloatValue(script->settings[settingIndex].value, outValue);}
                }
                else {
                    auto it = std::find_if(script->settings.begin(), script->settings.end(), [&](const ScriptSetting& setting) { return setting.key == settingKey; });
                    if (it != script->settings.end()) {return TryParseFloatValue(it->value, outValue);}
                }
            }
        }   return false;
    }
    inline bool WriteProperty(SceneObject& obj, const std::string& propertyId, float value) {
        if (propertyId == "localPosition.x") { obj.localPosition.x = value; obj.localInitialized = true; return true; }
        if (propertyId == "localPosition.y") { obj.localPosition.y = value; obj.localInitialized = true; return true; }
        if (propertyId == "localPosition.z") { obj.localPosition.z = value; obj.localInitialized = true; return true; }
        if (propertyId == "localRotation.x") { obj.localRotation.x = value; obj.localInitialized = true; return true; }
        if (propertyId == "localRotation.y") { obj.localRotation.y = value; obj.localInitialized = true; return true; }
        if (propertyId == "localRotation.z") { obj.localRotation.z = value; obj.localInitialized = true; return true; }
        if (propertyId == "localScale.x") { obj.localScale.x = std::max(0.0001f, value); obj.localInitialized = true; return true; }
        if (propertyId == "localScale.y") { obj.localScale.y = std::max(0.0001f, value); obj.localInitialized = true; return true; }
        if (propertyId == "localScale.z") { obj.localScale.z = std::max(0.0001f, value); obj.localInitialized = true; return true; }
        if (propertyId == "UI.Position.x" && obj.hasUI) { obj.ui.position.x = value; return true; }
        if (propertyId == "UI.Position.y" && obj.hasUI) { obj.ui.position.y = value; return true; }
        if ((propertyId == "UI.Size.x" || propertyId == "UI.Size.y") && obj.hasUI) {
            const float minUiSize = (obj.ui.type == UIElementType::Image || obj.ui.type == UIElementType::Sprite2D) ? 0.01f : 1.0f;
            if (propertyId == "UI.Size.x") {obj.ui.size.x = std::max(minUiSize, value);}
            else {obj.ui.size.y = std::max(minUiSize, value);}
            return true;
        }
        if (propertyId == "UI.Rotation" && obj.hasUI) { obj.ui.rotation = value; return true; }
        if (propertyId == "UI.SliderValue" && obj.hasUI) { obj.ui.sliderValue = std::clamp(value, obj.ui.sliderMin, obj.ui.sliderMax); return true; }
        if (propertyId == "UI.TextScale" && obj.hasUI) { obj.ui.textScale = std::max(0.01f, value); return true; }
        if (propertyId == "UI.Color.r" && obj.hasUI) { obj.ui.color.r = value; return true; }
        if (propertyId == "UI.Color.g" && obj.hasUI) { obj.ui.color.g = value; return true; }
        if (propertyId == "UI.Color.b" && obj.hasUI) { obj.ui.color.b = value; return true; }
        if (propertyId == "UI.Color.a" && obj.hasUI) { obj.ui.color.a = value; return true; }
        if (propertyId == "UI.Interactable" && obj.hasUI) { obj.ui.interactable = value >= 0.5f; return true; }
        if (propertyId == "UI.Anchor" && obj.hasUI) { obj.ui.anchor = static_cast<UIAnchor>(std::clamp(static_cast<int>(std::round(value)), 0, 4)); return true; }
        if (propertyId == "UI.RenderIn3D" && obj.hasUI) { obj.ui.renderIn3D = value >= 0.5f; return true; }
        if (propertyId == "UI.SpriteSheetEnabled" && obj.hasUI) { obj.ui.spriteSheetEnabled = value >= 0.5f; return true; }
        if (propertyId == "UI.SpriteCustomFramesEnabled" && obj.hasUI) { obj.ui.spriteCustomFramesEnabled = value >= 0.5f; return true; }
        if (propertyId == "UI.MaskChildren" && obj.hasUI) { obj.ui.maskChildren = value >= 0.5f; return true; }
        if (propertyId == "UI.SpriteFrame" && obj.hasUI) {
            int frameCount = 1;
            if (obj.ui.spriteCustomFramesEnabled && !obj.ui.spriteCustomFrames.empty()) {frameCount = static_cast<int>(obj.ui.spriteCustomFrames.size());}
            else {frameCount = std::max(1, obj.ui.spriteSheetColumns * obj.ui.spriteSheetRows);}
            obj.ui.spriteSheetFrame = std::clamp(static_cast<int>(std::round(value)), 0, std::max(0, frameCount - 1));
            return true;
        }
        if (propertyId == "UI.SpriteSheetFPS" && obj.hasUI) { obj.ui.spriteSheetFps = std::clamp(value, 1.0f, 120.0f); return true; }
        if (propertyId == "UI.SpriteSheetLoop" && obj.hasUI) { obj.ui.spriteSheetLoop = value >= 0.5f; return true; }
        if (propertyId == "UI.TextEffectFlags" && obj.hasUI) { obj.ui.textEffectFlags = static_cast<int>(std::round(value)); return true; }
        if (propertyId == "UI.TextEffectSpeed" && obj.hasUI) { obj.ui.textEffectSpeed = value; return true; }
        if (propertyId == "UI.TextEffectIntensity" && obj.hasUI) { obj.ui.textEffectIntensity = value; return true; }
        if (propertyId == "UI.ReceiveLighting2D" && obj.hasUI) { obj.ui.receiveLighting2D = value >= 0.5f; return true; }
        if (propertyId == "UI.UnlitLighting2D" && obj.hasUI) { obj.ui.unlitLighting2D = value >= 0.5f; return true; }
        if (propertyId == "UI.EmissiveLighting2D" && obj.hasUI) { obj.ui.emissiveLighting2D = value; return true; }
        if (propertyId == "Light.Color.r" && obj.hasLight) { obj.light.color.r = value; return true; }
        if (propertyId == "Light.Color.g" && obj.hasLight) { obj.light.color.g = value; return true; }
        if (propertyId == "Light.Color.b" && obj.hasLight) { obj.light.color.b = value; return true; }
        if (propertyId == "Light.Intensity" && obj.hasLight) { obj.light.intensity = value; return true; }
        if (propertyId == "Light.Range" && obj.hasLight) { obj.light.range = std::max(0.0f, value); return true; }
        if (propertyId == "Light.InnerAngle" && obj.hasLight) { obj.light.innerAngle = std::clamp(value, 0.0f, 180.0f); return true; }
        if (propertyId == "Light.OuterAngle" && obj.hasLight) { obj.light.outerAngle = std::clamp(value, 0.0f, 180.0f); return true; }
        if (propertyId == "Light.Enabled" && obj.hasLight) { obj.light.enabled = value >= 0.5f; return true; }
        if (propertyId == "Camera.FOV" && obj.hasCamera) { obj.camera.fov = std::clamp(value, 1.0f, 179.0f); return true; }
        if (propertyId == "Camera.NearClip" && obj.hasCamera) { obj.camera.nearClip = std::max(0.001f, value); return true; }
        if (propertyId == "Camera.FarClip" && obj.hasCamera) { obj.camera.farClip = std::max(obj.camera.nearClip + 0.01f, value); return true; }
        if (propertyId == "Camera.PixelsPerUnit" && obj.hasCamera) { obj.camera.pixelsPerUnit = std::max(1.0f, value); return true; }
        if (propertyId == "Camera.ApplyPostFX" && obj.hasCamera) { obj.camera.applyPostFX = value >= 0.5f; return true; }
        if (propertyId == "Camera.Use2D" && obj.hasCamera) { obj.camera.use2D = value >= 0.5f; return true; }
        if (propertyId == "PostFX.BloomIntensity" && obj.hasPostFX) { obj.postFx.bloomIntensity = std::max(0.0f, value); return true; }
        if (propertyId == "PostFX.Exposure" && obj.hasPostFX) { obj.postFx.exposure = value; return true; }
        if (propertyId == "PostFX.Contrast" && obj.hasPostFX) { obj.postFx.contrast = std::max(0.0f, value); return true; }
        if (propertyId == "PostFX.Saturation" && obj.hasPostFX) { obj.postFx.saturation = std::max(0.0f, value); return true; }
        if (propertyId == "PostFX.ColorFilter.r" && obj.hasPostFX) { obj.postFx.colorFilter.r = value; return true; }
        if (propertyId == "PostFX.ColorFilter.g" && obj.hasPostFX) { obj.postFx.colorFilter.g = value; return true; }
        if (propertyId == "PostFX.ColorFilter.b" && obj.hasPostFX) { obj.postFx.colorFilter.b = value; return true; }
        if (propertyId == "PostFX.Enabled" && obj.hasPostFX) { obj.postFx.enabled = value >= 0.5f; return true; }
        if (propertyId == "Material.Color.r") { obj.material.color.r = value; return true; }
        if (propertyId == "Material.Color.g") { obj.material.color.g = value; return true; }
        if (propertyId == "Material.Color.b") { obj.material.color.b = value; return true; }
        if (propertyId == "Material.Alpha") { obj.material.alpha = std::clamp(value, 0.0f, 1.0f); return true; }
        if (propertyId == "Material.AmbientStrength") { obj.material.ambientStrength = value; return true; }
        if (propertyId == "Material.SpecularStrength") { obj.material.specularStrength = value; return true; }
        if (propertyId == "Material.Shininess") { obj.material.shininess = value; return true; }
        if (propertyId == "Material.NormalMapIntensity") { obj.material.normalMapIntensity = std::clamp(value, 0.0f, 2.0f); return true; }
        if (propertyId == "Material.TextureMix") { obj.material.textureMix = value; return true; }
        if (propertyId == "Rigidbody.Mass" && obj.hasRigidbody) { obj.rigidbody.mass = std::max(0.001f, value); return true; }
        if (propertyId == "Rigidbody.UseGravity" && obj.hasRigidbody) { obj.rigidbody.useGravity = value >= 0.5f; return true; }
        if (propertyId == "Rigidbody.IsKinematic" && obj.hasRigidbody) { obj.rigidbody.isKinematic = value >= 0.5f; return true; }
        if (propertyId == "Rigidbody.LinearDamping" && obj.hasRigidbody) { obj.rigidbody.linearDamping = std::max(0.0f, value); return true; }
        if (propertyId == "Rigidbody.AngularDamping" && obj.hasRigidbody) { obj.rigidbody.angularDamping = std::max(0.0f, value); return true; }
        if (propertyId == "Rigidbody.LockRotationX" && obj.hasRigidbody) { obj.rigidbody.lockRotationX = value >= 0.5f; return true; }
        if (propertyId == "Rigidbody.LockRotationY" && obj.hasRigidbody) { obj.rigidbody.lockRotationY = value >= 0.5f; return true; }
        if (propertyId == "Rigidbody.LockRotationZ" && obj.hasRigidbody) { obj.rigidbody.lockRotationZ = value >= 0.5f; return true; }
        if (propertyId == "Audio.Volume" && obj.hasAudioSource) { obj.audioSource.volume = std::clamp(value, 0.0f, 2.0f); return true; }
        if (propertyId == "Audio.MinDistance" && obj.hasAudioSource) { obj.audioSource.minDistance = std::max(0.01f, value); return true; }
        if (propertyId == "Audio.MaxDistance" && obj.hasAudioSource) { obj.audioSource.maxDistance = std::max(obj.audioSource.minDistance + 0.01f, value); return true; }
        if (propertyId == "Audio.Loop" && obj.hasAudioSource) { obj.audioSource.loop = value >= 0.5f; return true; }
        if (propertyId == "Audio.Spatial" && obj.hasAudioSource) {
            obj.audioSource.spatial = value >= 0.5f;
            obj.audioSource.spatialBlend = obj.audioSource.spatial ? 1.0f : 0.0f;
            return true;
        }
        if (propertyId == "AIAgent.Speed" && obj.hasAIAgent) { obj.aiAgent.speed = std::max(0.05f, value); return true; }
        if (propertyId == "AIAgent.StoppingDistance" && obj.hasAIAgent) { obj.aiAgent.stoppingDistance = std::max(0.0f, value); return true; }
        int scriptId = -1;
        bool legacyIndex = false;
        int settingIndex = -1;
        std::string settingKey;
        if (ParseScriptSettingPropertyId(propertyId, scriptId, legacyIndex, settingIndex, settingKey)) {
            ScriptComponent* script = FindScriptByInspectorId(obj, scriptId);
            if (!script && scriptId >= 0 && scriptId < static_cast<int>(obj.scripts.size())) {script = &obj.scripts[static_cast<size_t>(scriptId)];}
            if (script) {
                if (legacyIndex) {
                    if (settingIndex >= 0 && settingIndex < static_cast<int>(script->settings.size())) {
                        char buffer[64];
                        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
                        script->settings[settingIndex].value = buffer;
                        return true;
                    }
                } else {
                    auto it = std::find_if(script->settings.begin(), script->settings.end(),
                        [&](ScriptSetting& setting) { return setting.key == settingKey; });
                    if (it != script->settings.end()) {
                        char buffer[64];
                        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
                        it->value = buffer;
                        return true;
                    }
                }
            }
        }   return false;
    }
    inline float GetPropertyThreshold(const std::string& propertyId) {
        if (propertyId == "Light.Enabled" ||
            propertyId == "Audio.Loop" ||
            propertyId == "Audio.Spatial" ||
            propertyId == "UI.Interactable" ||
            propertyId == "UI.RenderIn3D" ||
            propertyId == "UI.SpriteSheetEnabled" ||
            propertyId == "UI.SpriteCustomFramesEnabled" ||
            propertyId == "UI.MaskChildren" ||
            propertyId == "UI.SpriteSheetLoop" ||
            propertyId == "UI.Anchor" ||
            propertyId == "UI.ReceiveLighting2D" ||
            propertyId == "UI.UnlitLighting2D" ||
            propertyId == "UI.TextEffectFlags" ||
            propertyId == "PostFX.Enabled" ||
            propertyId == "Camera.ApplyPostFX" ||
            propertyId == "Camera.Use2D" ||
            propertyId == "Rigidbody.UseGravity" ||
            propertyId == "Rigidbody.IsKinematic" ||
            propertyId == "Rigidbody.LockRotationX" ||
            propertyId == "Rigidbody.LockRotationY" ||
            propertyId == "Rigidbody.LockRotationZ") {
            return 0.5f;
        }
        if (propertyId.rfind("ScriptSetting.", 0) == 0) {return 0.0005f;}
        if (propertyId.rfind("localRotation.", 0) == 0) {return 0.05f;}
        if (propertyId.rfind("localPosition.", 0) == 0 || propertyId.rfind("localScale.", 0) == 0) {return 0.0005f;}
        return 0.001f;
    }
    inline std::vector<std::string> EnumerateProperties(const SceneObject& obj) {
        std::vector<std::string> props;
        props.reserve(64 + obj.scripts.size() * 8);
        props.push_back("localPosition.x");
        props.push_back("localPosition.y");
        props.push_back("localPosition.z");
        props.push_back("localRotation.x");
        props.push_back("localRotation.y");
        props.push_back("localRotation.z");
        props.push_back("localScale.x");
        props.push_back("localScale.y");
        props.push_back("localScale.z");
        if (obj.hasUI) {
            props.push_back("UI.Position.x");
            props.push_back("UI.Position.y");
            props.push_back("UI.Size.x");
            props.push_back("UI.Size.y");
            props.push_back("UI.Rotation");
            props.push_back("UI.SliderValue");
            props.push_back("UI.TextScale");
            props.push_back("UI.Color.r");
            props.push_back("UI.Color.g");
            props.push_back("UI.Color.b");
            props.push_back("UI.Color.a");
            props.push_back("UI.Interactable");
            props.push_back("UI.Anchor");
            props.push_back("UI.RenderIn3D");
            props.push_back("UI.SpriteSheetEnabled");
            props.push_back("UI.SpriteCustomFramesEnabled");
            props.push_back("UI.MaskChildren");
            props.push_back("UI.SpriteFrame");
            props.push_back("UI.SpriteSheetFPS");
            props.push_back("UI.SpriteSheetLoop");
            props.push_back("UI.TextEffectFlags");
            props.push_back("UI.TextEffectSpeed");
            props.push_back("UI.TextEffectIntensity");
            props.push_back("UI.ReceiveLighting2D");
            props.push_back("UI.UnlitLighting2D");
            props.push_back("UI.EmissiveLighting2D");
        }
        if (obj.hasLight) {
            props.push_back("Light.Color.r");
            props.push_back("Light.Color.g");
            props.push_back("Light.Color.b");
            props.push_back("Light.Intensity");
            props.push_back("Light.Range");
            props.push_back("Light.InnerAngle");
            props.push_back("Light.OuterAngle");
            props.push_back("Light.Enabled");
        }
        if (obj.hasCamera) {
            props.push_back("Camera.FOV");
            props.push_back("Camera.NearClip");
            props.push_back("Camera.FarClip");
            props.push_back("Camera.PixelsPerUnit");
            props.push_back("Camera.ApplyPostFX");
            props.push_back("Camera.Use2D");
        }
        if (obj.hasPostFX) {
            props.push_back("PostFX.BloomIntensity");
            props.push_back("PostFX.Exposure");
            props.push_back("PostFX.Contrast");
            props.push_back("PostFX.Saturation");
            props.push_back("PostFX.ColorFilter.r");
            props.push_back("PostFX.ColorFilter.g");
            props.push_back("PostFX.ColorFilter.b");
            props.push_back("PostFX.Enabled");
        }
        props.push_back("Material.Color.r");
        props.push_back("Material.Color.g");
        props.push_back("Material.Color.b");
        props.push_back("Material.Alpha");
        props.push_back("Material.AmbientStrength");
        props.push_back("Material.SpecularStrength");
        props.push_back("Material.Shininess");
        props.push_back("Material.TextureMix");
        if (obj.hasRigidbody) {
            props.push_back("Rigidbody.Mass");
            props.push_back("Rigidbody.UseGravity");
            props.push_back("Rigidbody.IsKinematic");
            props.push_back("Rigidbody.LinearDamping");
            props.push_back("Rigidbody.AngularDamping");
            props.push_back("Rigidbody.LockRotationX");
            props.push_back("Rigidbody.LockRotationY");
            props.push_back("Rigidbody.LockRotationZ");
        }
        if (obj.hasAudioSource) {
            props.push_back("Audio.Volume");
            props.push_back("Audio.MinDistance");
            props.push_back("Audio.MaxDistance");
            props.push_back("Audio.Loop");
            props.push_back("Audio.Spatial");
        }
        if (obj.hasAIAgent) {
            props.push_back("AIAgent.Speed");
            props.push_back("AIAgent.StoppingDistance");
        }
        for (const ScriptComponent& script : obj.scripts) {
            if (script.inspectorId <= 0) continue;
            for (const ScriptSetting& setting : script.settings) {
                float parsed = 0.0f;
                if (!TryParseFloatValue(setting.value, parsed)) {continue;}
                props.push_back(MakeScriptSettingPropertyId(script.inspectorId, setting.key));
            }
        }   return props;
    }
}