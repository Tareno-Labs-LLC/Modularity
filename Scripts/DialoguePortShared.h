#pragma once
#include "ModuCPPScriptApi.h"
#include "ModuInputScriptApi.h"
#include "ModuEngineScriptApi.h"
#include "ModuCPPExperimentalScriptApi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>
#include <GLFW/glfw3.h>
namespace DialoguePort {
    enum class Language {
        English = 0,
        German = 1,
        JapaneseKana = 2
    };
    enum class TextEffectType : int {
        None = 0,
        Wave = 1 << 0,
        Shake = 1 << 1,
        Bounce = 1 << 2,
        Rotate = 1 << 3,
        Fade = 1 << 4
    };
    inline constexpr const char* kInteractionRequestPending = "interaction.requestPending";
    inline constexpr const char* kInteractionOverrideLines = "interaction.overrideDialogueLines";
    inline constexpr const char* kInteractionEndEnable = "interaction.itemsToEnableOnEnd";
    inline constexpr const char* kInteractionEndDisable = "interaction.itemsToDisableOnEnd";
    inline constexpr const char* kInteractionPlayerRef = "interaction.playerRef";
    inline constexpr const char* kInteractionRequestSerial = "interaction.requestSerial";
    struct DialogueLine {
        std::string characterName;
        std::string sentence;
        std::string sentenceGerman;
        std::string sentenceJapaneseKana;
        std::string characterSoundClip;
        float typingSpeed = 0.03f;
        TextEffectType textEffect = TextEffectType::None;
        float animationSpeed = 1.0f;
        float effectIntensity = 1.0f;
        std::string notTalkingObjectRef;
        std::string openMouthObjectRef;
        std::string closedMouthObjectRef;
        std::vector<std::string> itemsToEnable;
        std::vector<std::string> itemsToDisable;
    };
    using ::ModuCPP::Trim;
    using ::ModuCPP::ParseInt;
    using ::ModuCPP::ParseFloat;
    using ::ModuCPP::ParseBool;
    using ::ModuCPP::GetScriptSetting;
    using ::ModuCPP::SetScriptSetting;
    using ::ModuCPP::EscapeField;
    using ::ModuCPP::UnescapeField;
    using ::ModuCPP::SplitEscaped;
    using ::ModuCPP::JoinEscaped;
    using ::ModuCPP::DeserializeObjectRefs;
    using ::ModuCPP::SerializeObjectRefs;
    inline std::string SerializeDialogueLines(const std::vector<DialogueLine>& lines) {
        std::vector<std::string> encodedLines;
        encodedLines.reserve(lines.size());
        for (const DialogueLine& line : lines) {
            std::vector<std::string> fields;
            fields.reserve(14);
            fields.push_back(line.characterName);
            fields.push_back(line.sentence);
            fields.push_back(line.sentenceGerman);
            fields.push_back(line.sentenceJapaneseKana);
            fields.push_back(line.characterSoundClip);
            fields.push_back(std::to_string(line.typingSpeed));
            fields.push_back(std::to_string(static_cast<int>(line.textEffect)));
            fields.push_back(std::to_string(line.animationSpeed));
            fields.push_back(std::to_string(line.effectIntensity));
            fields.push_back(line.notTalkingObjectRef);
            fields.push_back(line.openMouthObjectRef);
            fields.push_back(line.closedMouthObjectRef);
            fields.push_back(SerializeObjectRefs(line.itemsToEnable));
            fields.push_back(SerializeObjectRefs(line.itemsToDisable));
            encodedLines.push_back(JoinEscaped(fields, '|'));
        }
        return JoinEscaped(encodedLines, '\t'); // Use tab as the outer delimiter so values stay single-line in scene files.
    }
    inline std::vector<DialogueLine> DeserializeDialogueLines(const std::string& encoded) {
        std::vector<DialogueLine> lines;
        if (encoded.empty()) return lines;
        std::vector<std::string> rawLines = SplitEscaped(encoded, '\t');
        if (rawLines.size() <= 1 && encoded.find('\t') == std::string::npos && // Backward compatibility for older saved data that used newline delimiters.
            encoded.find('\n') != std::string::npos) {
            rawLines = SplitEscaped(encoded, '\n');
        }   lines.reserve(rawLines.size());
        for (const std::string& rawLine : rawLines) {
            if (Trim(rawLine).empty()) continue;
            std::vector<std::string> fields = SplitEscaped(rawLine, '|');
            if (fields.empty()) continue;
            DialogueLine line;
            if (fields.size() > 0) line.characterName = fields[0];
            if (fields.size() > 1) line.sentence = fields[1];
            if (fields.size() > 2) line.sentenceGerman = fields[2];
            if (fields.size() > 3) line.sentenceJapaneseKana = fields[3];
            if (fields.size() > 4) line.characterSoundClip = fields[4];
            if (fields.size() > 5) line.typingSpeed = ParseFloat(fields[5], line.typingSpeed);
            if (fields.size() > 6) line.textEffect = static_cast<TextEffectType>(ParseInt(fields[6], 0));
            if (fields.size() > 7) line.animationSpeed = ParseFloat(fields[7], line.animationSpeed);
            if (fields.size() > 8) line.effectIntensity = ParseFloat(fields[8], line.effectIntensity);
            if (fields.size() > 9) line.notTalkingObjectRef = fields[9];
            if (fields.size() > 10) line.openMouthObjectRef = fields[10];
            if (fields.size() > 11) line.closedMouthObjectRef = fields[11];
            if (fields.size() > 12) line.itemsToEnable = DeserializeObjectRefs(fields[12]);
            if (fields.size() > 13) line.itemsToDisable = DeserializeObjectRefs(fields[13]);
            lines.push_back(std::move(line));
        }   return lines;
    }
    inline std::string ToLowerAscii(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }); return value;
    }
    inline std::string GetSentenceForLanguage(const DialogueLine& line, Language language) {
        switch (language) {
            case Language::English:      return line.sentence;
            case Language::German:       return line.sentenceGerman.empty()       ? line.sentence : line.sentenceGerman;
            case Language::JapaneseKana: return line.sentenceJapaneseKana.empty() ? line.sentence : line.sentenceJapaneseKana;
            default:                     return line.sentence;
        }
    }
    inline std::string ParseSentenceForDisplay(const std::string& sentence) {
        static const std::regex taggedWordPattern(R"(\(\[(\w+),\s*([\d.]+),\s*([\d.]+),\](.*?)\))");
        std::string clean;
        clean.reserve(sentence.size());
        size_t lastIndex = 0;
        for (std::sregex_iterator it(sentence.begin(), sentence.end(), taggedWordPattern), end; it != end; ++it) {
            const std::smatch& match = *it;
            const size_t matchPos = static_cast<size_t>(match.position());
            const size_t matchLen = static_cast<size_t>(match.length());
            if (matchPos > lastIndex) clean += sentence.substr(lastIndex, matchPos - lastIndex);
            if (match.size() >= 5) clean += match[4].str();
            lastIndex = matchPos + matchLen;
        }
        if (lastIndex < sentence.size()) clean += sentence.substr(lastIndex);
        return clean;
    }
    using ::ModuCPP::MakeObjectRef;
    using ::ModuCPP::ResolveSceneObjectRef;
    inline bool IsDialogueSystemScript(const ScriptComponent& script) {
        const std::string pathLower = ToLowerAscii(script.path);
        const std::string managedTypeLower = ToLowerAscii(script.managedType);
        return pathLower.find("dialoguesystem") != std::string::npos || managedTypeLower.find("dialoguesystem") != std::string::npos;
    }
    struct DialogueScriptTarget {
        SceneObject* object = nullptr;
        ScriptComponent* script = nullptr;
    };
    inline ScriptComponent* FindDialogueSystemScriptOnObject(SceneObject* object) {
        if (!object) return nullptr;
        for (ScriptComponent& script : object->scripts) {
            if (IsDialogueSystemScript(script)) return &script;
        }   return nullptr;
    }
    inline DialogueScriptTarget FindDialogueScriptTarget(ScriptContext& ctx, SceneObject* object) {
        if (!object) return {};
        if (ScriptComponent* direct = FindDialogueSystemScriptOnObject(object)) return DialogueScriptTarget{object, direct};
        std::vector<int> pendingChildIds = object->childIds;
        int childGuard = 0;
        while (!pendingChildIds.empty() && childGuard < 512) {
            const int childId = pendingChildIds.back();
            pendingChildIds.pop_back();
            ++childGuard;
            SceneObject* child = ctx.FindObjectById(childId);
            if (!child) continue;
            if (ScriptComponent* childScript = FindDialogueSystemScriptOnObject(child)) {
                return DialogueScriptTarget{child, childScript};
            }
            pendingChildIds.insert(pendingChildIds.end(), child->childIds.begin(), child->childIds.end());
        }
        int parentId = object->parentId;
        int parentGuard = 0;
        while (parentId >= 0 && parentGuard < 256) {
            ++parentGuard;
            SceneObject* parent = ctx.FindObjectById(parentId);
            if (!parent) break;
            if (ScriptComponent* parentScript = FindDialogueSystemScriptOnObject(parent)) return DialogueScriptTarget{parent, parentScript};
            parentId = parent->parentId;
        }   return {};
    }
    using ::ModuCPP::SetObjectEnabledState;
    using ::ModuCPP::SetObjectsEnabledState;
    using ::ModuCPP::GetObjectReferencePosition;
    using ::ModuCPP::GetCurrentObjectName;
    using ::ModuCPP::TryPlayAnimationClipNamed;
    using ::ModuCPP::SetUITextLabel;

    inline bool SetUITextEffects(ScriptContext& ctx, const std::string& objectRef, TextEffectType effect, float animationSpeed, float effectIntensity) {
        return ModuCPP::SetUITextEffects(ctx, objectRef, static_cast<int>(effect), animationSpeed, effectIntensity);
    }
    using ::ModuCPP::SetRigidbody2DSimulated;
    using ::ModuCPP::DrawStdStringInput;
    using ::ModuCPP::DrawObjectRefInput;
    using ::ModuCPP::DrawAudioClipInput;
    using ::ModuCPP::DrawObjectRefListEditor;
    using ::ModuCPP::IsRuntimeKeyDown;
    using ::ModuCPP::IsSubmitDown;
    inline const char* LanguageLabel(Language language) {
        switch (language) {
            case Language::English: return "English";
            case Language::German: return "German";
            case Language::JapaneseKana: return "Japanese Kana";
            default: return "English";
        }
    }
    inline bool DrawLanguageCombo(const char* label, Language& language) {
        bool changed = false;
        if (ImGui::BeginCombo(label, LanguageLabel(language))) {
            std::array<Language, 3> values = {
                Language::English,
                Language::German,
                Language::JapaneseKana
            };
            for (Language value : values) {
                const bool selected = (language == value);
                if (ImGui::Selectable(LanguageLabel(value), selected)) {
                    language = value;
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }   ImGui::EndCombo();
        }   return changed;
    }
    inline void DrawTextEffectFlagsEditor(TextEffectType& effect) {
        int bits = static_cast<int>(effect);
        auto drawToggle = [&](const char* label, TextEffectType flag, bool sameLineAfter = false) {
            bool enabled = (bits & static_cast<int>(flag)) != 0;
            if (ImGui::Checkbox(label, &enabled)) {
                if (enabled) bits |= static_cast<int>(flag);
                else bits &= ~static_cast<int>(flag);
            }
            if (sameLineAfter) ImGui::SameLine();
        };
        drawToggle("Wave", TextEffectType::Wave, true);
        drawToggle("Shake", TextEffectType::Shake, true);
        drawToggle("Bounce", TextEffectType::Bounce);
        drawToggle("Rotate", TextEffectType::Rotate, true);
        drawToggle("Fade", TextEffectType::Fade);
        effect = static_cast<TextEffectType>(bits);
    }
    inline bool DrawDialogueLineToolbar(std::vector<DialogueLine>& lines, int& selectedIndex, const char* addLabel, const char* removeLabel) {
        bool changed = false;
        if (ImGui::Button(addLabel)) {
            lines.push_back(lines.empty() ? DialogueLine{} : lines.back());
            selectedIndex = static_cast<int>(lines.size()) - 1;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(removeLabel) &&
            selectedIndex >= 0 &&
            selectedIndex < static_cast<int>(lines.size())) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(selectedIndex));
            selectedIndex = lines.empty() ? -1 : std::clamp(selectedIndex, 0, static_cast<int>(lines.size()) - 1);
            changed = true;
        }
        return changed;
    }
    inline void DrawDialogueLineList(const std::vector<DialogueLine>& lines, int& selectedIndex, const char* childId) {
        ImGui::BeginChild(childId, ImVec2(230.0f, 220.0f), true);
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string label = std::to_string(i + 1) + ". " + (lines[i].characterName.empty() ? std::string("<Unnamed>") : lines[i].characterName);
            if (ImGui::Selectable(label.c_str(), selectedIndex == static_cast<int>(i))) {
                selectedIndex = static_cast<int>(i);
            }
        }   ImGui::EndChild();
    }
    inline bool DrawDialogueLineEditor(ScriptContext& ctx, std::vector<DialogueLine>& lines, int& selectedIndex, const char* addLabel = "Add Line",
                                    const char* removeLabel = "Remove Line", const char* emptyText = "No dialogue lines configured.", const char* childId = "DialogueLinesList") {
        bool changed = DrawDialogueLineToolbar(lines, selectedIndex, addLabel, removeLabel);
        if (lines.empty()) {
            ImGui::TextDisabled("%s", emptyText);
            return changed;
        }
        selectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(lines.size()) - 1);
        DrawDialogueLineList(lines, selectedIndex, childId);
        ImGui::SameLine();
        ImGui::BeginGroup();
        DialogueLine& line = lines[static_cast<size_t>(selectedIndex)];
        changed |= DrawStdStringInput("Character Name", line.characterName, 256);
        changed |= DrawStdStringInput("Sentence (EN)", line.sentence, 2048, 0, true, 60.0f);
        changed |= DrawStdStringInput("Sentence (DE)", line.sentenceGerman, 2048, 0, true, 60.0f);
        changed |= DrawStdStringInput("Sentence (JP Kana)", line.sentenceJapaneseKana, 2048, 0, true, 60.0f);
        changed |= DrawAudioClipInput("Character Voice Clip", line.characterSoundClip, 512);
        changed |= ImGui::DragFloat("Typing Speed", &line.typingSpeed, 0.001f, 0.001f, 1.0f, "%.3f");
        changed |= ImGui::DragFloat("Animation Speed", &line.animationSpeed, 0.01f, 0.01f, 10.0f, "%.2f");
        changed |= ImGui::DragFloat("Effect Intensity", &line.effectIntensity, 0.01f, 0.0f, 10.0f, "%.2f");
        ImGui::TextUnformatted("Text Effects");
        const TextEffectType before = line.textEffect;
        DrawTextEffectFlagsEditor(line.textEffect);
        changed |= (before != line.textEffect);
        changed |= DrawObjectRefInput(ctx, "Not Talking Object", line.notTalkingObjectRef);
        changed |= DrawObjectRefInput(ctx, "Open Mouth Object", line.openMouthObjectRef);
        changed |= DrawObjectRefInput(ctx, "Closed Mouth Object", line.closedMouthObjectRef);
        changed |= DrawObjectRefListEditor(ctx, "Line Items To Enable", line.itemsToEnable);
        changed |= DrawObjectRefListEditor(ctx, "Line Items To Disable", line.itemsToDisable);
        ImGui::EndGroup();
        return changed;
    }
    template <typename RuntimeStateT>
    inline void DrawDialogueRuntimeStatus(const RuntimeStateT* state) {
        if (!state) {
            ImGui::TextDisabled("Runtime: idle"); return;
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Runtime");
        ImGui::TextDisabled("Running: %s", state->running ? "Yes" : "No");
        ImGui::TextDisabled("Typing: %s", state->isTyping ? "Yes" : "No");
        ImGui::TextDisabled("Line: %d", state->index + 1);
        ImGui::TextDisabled("Chars: %zu / %zu", state->revealedCharacters, state->currentCleanSentence.size());
    }
}
