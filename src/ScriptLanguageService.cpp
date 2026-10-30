#include "ScriptLanguageService.h"
#include "ModuCPPLanguagePack.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
static const size_t kMaxIncludeFileBytes = 256 * 1024; // 256kb cap per file
static const size_t kMaxIncludeFiles = 128;

namespace {
    static std::string toLowerCopy(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    static std::string trimLeft(const std::string& value) {
        size_t start = value.find_first_not_of(" \t");
        if (start == std::string::npos) return "";
        return value.substr(start);
    }

    static std::string extensionLower(const fs::path& path) {
        return toLowerCopy(path.extension().string());
    }

    static bool isIdentifierStart(unsigned char c) {
        return std::isalpha(c) || c == '_';
    }

    static bool isIdentifierBody(unsigned char c) {
        return std::isalnum(c) || c == '_';
    }

    static std::string parseDefineName(const std::string& defineLine) {
        size_t i = 0;
        while (i < defineLine.size() && std::isspace(static_cast<unsigned char>(defineLine[i]))) {
            ++i;
        }
        if (i >= defineLine.size() || !isIdentifierStart(static_cast<unsigned char>(defineLine[i]))) {
            return {};
        }
        size_t start = i++;
        while (i < defineLine.size() && isIdentifierBody(static_cast<unsigned char>(defineLine[i]))) {
            ++i;
        }
        return defineLine.substr(start, i - start);
    }

    static std::string trimCopy(const std::string& value) {
        std::string trimmed = trimLeft(value);
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
            trimmed.pop_back();
        }
        return trimmed;
    }

    static std::string normalizeModuCppMemberAccess(std::string value) {
        size_t pos = 0;
        while ((pos = value.find("::", pos)) != std::string::npos) {
            value.replace(pos, 2, ".");
            ++pos;
        }
        return value;
    }

    static std::vector<std::string> splitTopLevelComma(const std::string& text) {
        std::vector<std::string> parts;
        std::string current;
        int parenDepth = 0;
        int angleDepth = 0;
        int bracketDepth = 0;
        int braceDepth = 0;
        bool inString = false;
        bool inChar = false;
        bool escaped = false;

        for (char c : text) {
            if (inString || inChar) {
                current.push_back(c);
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if ((inString && c == '"') || (inChar && c == '\'')) {
                    inString = false;
                    inChar = false;
                }
                continue;
            }
            if (c == '"') {
                inString = true;
                current.push_back(c);
                continue;
            }
            if (c == '\'') {
                inChar = true;
                current.push_back(c);
                continue;
            }

            if (c == '(') ++parenDepth;
            else if (c == ')') parenDepth = std::max(0, parenDepth - 1);
            else if (c == '<') ++angleDepth;
            else if (c == '>') angleDepth = std::max(0, angleDepth - 1);
            else if (c == '[') ++bracketDepth;
            else if (c == ']') bracketDepth = std::max(0, bracketDepth - 1);
            else if (c == '{') ++braceDepth;
            else if (c == '}') braceDepth = std::max(0, braceDepth - 1);

            if (c == ',' && parenDepth == 0 && angleDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
                parts.push_back(trimCopy(current));
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        parts.push_back(trimCopy(current));
        return parts;
    }

    static std::string identifierTailFromExpression(const std::string& expression) {
        std::string expr = trimCopy(expression);
        if (expr.empty()) return {};
        while (!expr.empty() && (expr.back() == ')' || expr.back() == ']' || expr.back() == ';')) {
            expr.pop_back();
            expr = trimCopy(expr);
        }
        size_t end = expr.size();
        size_t start = end;
        while (start > 0 && isIdentifierBody(static_cast<unsigned char>(expr[start - 1]))) {
            --start;
        }
        if (start >= end) return {};
        return expr.substr(start, end - start);
    }

    static std::vector<std::string> extractDefineIdentifiers(const std::string& text) {
        std::unordered_set<std::string> unique;
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) {
            std::string trimmed = trimLeft(line);
            if (trimmed.rfind("#define", 0) != 0) continue;
            std::string symbol = parseDefineName(trimmed.substr(7));
            if (!symbol.empty()) {
                unique.insert(symbol);
            }
        }
        std::vector<std::string> out(unique.begin(), unique.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    static std::vector<std::string> extractModuCppImports(const std::string& text) {
        std::unordered_set<std::string> unique;
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) {
            std::string trimmed = trimLeft(line);
            if (trimmed.rfind("add ", 0) != 0 && trimmed.rfind("#include ", 0) != 0) continue;
            size_t start = trimmed.find(' ');
            if (start == std::string::npos) continue;
            while (start < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[start]))) ++start;
            size_t end = start;
            while (end < trimmed.size()) {
                const char c = trimmed[end];
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != ':' && c != '.') break;
                ++end;
            }
            if (end > start) {
                unique.insert(normalizeModuCppMemberAccess(trimmed.substr(start, end - start)));
            }
        }
        std::vector<std::string> out(unique.begin(), unique.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    static void appendUnique(std::unordered_set<std::string>& unique, const std::string& value) {
        if (value.size() >= 2) {
            unique.insert(value);
        }
    }

    static std::vector<std::string> extractAutoFieldIdentifiers(const std::string& text) {
        std::unordered_set<std::string> unique;
        size_t pos = 0;
        while ((pos = text.find("AutoFields", pos)) != std::string::npos) {
            size_t open = text.find('(', pos + 10);
            if (open == std::string::npos) break;
            int depth = 1;
            size_t close = std::string::npos;
            for (size_t i = open + 1; i < text.size(); ++i) {
                if (text[i] == '(') ++depth;
                else if (text[i] == ')') {
                    --depth;
                    if (depth == 0) {
                        close = i;
                        break;
                    }
                }
            }
            if (close == std::string::npos) break;
            for (const std::string& part : splitTopLevelComma(text.substr(open + 1, close - open - 1))) {
                appendUnique(unique, identifierTailFromExpression(part));
            }
            pos = close + 1;
        }
        std::vector<std::string> out(unique.begin(), unique.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    static std::vector<std::string> extractVariableIdentifiers(const std::string& text,
                                                               ScriptLanguageServiceLanguage language,
                                                               const std::unordered_set<std::string>& keywords) {
        std::unordered_set<std::string> unique;

        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            // Strip leading whitespace
            size_t i = 0;
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i >= line.size()) continue;

            // Skip comments, preprocessor, and access specifiers
            if (line[i] == '/' || line[i] == '#') continue;

            // Must contain an = { ( ; or , to be a declaration line
            const bool hasAssignOrEnd = line.find('=') != std::string::npos ||
                                        line.find(';') != std::string::npos ||
                                        line.find('{') != std::string::npos;
            if (!hasAssignOrEnd) continue;

            // Skip lines that look like function definitions (contain a paren before any = or ;)
            const size_t parenPos = line.find('(');
            const size_t assignPos = line.find_first_of("=;{");
            if (parenPos != std::string::npos && parenPos < assignPos) continue;

            // Walk backwards from = or ; to find the variable name
            size_t nameEnd = assignPos;
            while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(line[nameEnd - 1]))) --nameEnd;
            if (nameEnd == 0) continue;

            // Must end with an identifier character
            if (!isIdentifierBody(static_cast<unsigned char>(line[nameEnd - 1]))) continue;

            size_t nameStart = nameEnd;
            while (nameStart > 0 && isIdentifierBody(static_cast<unsigned char>(line[nameStart - 1]))) --nameStart;

            if (nameEnd <= nameStart) continue;
            const std::string name = line.substr(nameStart, nameEnd - nameStart);

            // Must have a space before it (i.e. a type precedes it)
            if (nameStart == 0 || !std::isspace(static_cast<unsigned char>(line[nameStart - 1]))) continue;

            if (name.size() < 2) continue;
            if (keywords.find(name) != keywords.end()) continue;
            if (!isIdentifierStart(static_cast<unsigned char>(name[0]))) continue;

            unique.insert(name);
        }

        // ModuCPP each loop variables
        if (language == ScriptLanguageServiceLanguage::ModuCPP) {
            std::istringstream stream2(text);
            while (std::getline(stream2, line)) {
                // Match: each( [ref] <type> <name> in
                const size_t eachPos = line.find("each");
                if (eachPos == std::string::npos) continue;
                size_t i = eachPos + 4;
                while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                if (i >= line.size() || line[i] != '(') continue;
                ++i;
                while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                // optional ref
                if (line.substr(i, 3) == "ref") { i += 3; while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i; }
                // skip type
                while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                // read name
                size_t nameStart = i;
                while (i < line.size() && isIdentifierBody(static_cast<unsigned char>(line[i]))) ++i;
                if (i > nameStart) {
                    unique.insert(line.substr(nameStart, i - nameStart));
                }
            }
        }

        std::vector<std::string> out(unique.begin(), unique.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    static std::vector<std::string> moduCppBuiltins() {
        static const char* kBuiltins[] = {
            "add", "mark", "Calc", "to", "then", "ref", "each", "in", "SubScript", "AutoFields",
            "ModuCPP", "ModuCPP.Experimental",
            "ModuNode", "ModuBehaviour", "SceneObj", "SceneObject", "Col", "ScriptContext", "List", "IEnum",
            "string", "vec2", "vec3", "Vector2", "Vector3", "ConsoleMessageType", "UISliderStyle",
            "UIButtonStyle", "UIElementType", "Header", "Slider", "ObjectRef", "ObjectList",
            "DialogueLines", "ClipGridPair", "Separator", "SoundSet", "range", "step",
            "Begin", "TickUpdate", "Update", "Spec", "TestEditor", "RenderEditorWindow",
            "ExitRenderEditorWindow", "Script_OnInspector", "OnCollideEnter", "OnCollideHold",
            "OnCollideExit", "Config", "State", "BindSetting",
            "BindArray", "BindArray2D", "SerializeSubScript", "DeserializeSubScript",
            "SerializeSubScriptArray", "DeserializeSubScriptArray", "EditSubScript",
            "EditSubScriptArray", "SetFrameDeltaTime", "StartTimer", "TimerReady", "IntRD",
            "IntR", "IntRU", "AddLog", "Type", "MODU_SCRIPT", "GetProjectGravityScale", "SetProjectGravityScale",
            "EditFloat", "EditVec3", "EditBool", "EditInt", "EditString", "hasRigidbody2D",
            "getRigidbody2DVelocity", "setRigidbody2DVelocity", "moveTowards",
            "TryMoveRigidbody2D", "moveRigidbody2D", "movePosition2D", "warnOnce",
            "warnMissingComponentOnce", "hasAudioSource", "playSound", "EditClipSelector",
            "EditDirectionalClipGrid", "EditSoundSet", "KeyDown", "KeyPressed",
            "IsRuntimeKeyDown", "IsSubmitDown", "Trim", "ParseInt", "ParseFloat", "ParseBool",
            "GetScriptSetting", "SetScriptSetting", "EscapeField", "UnescapeField",
            "SplitEscaped", "JoinEscaped", "DeserializeObjectRefs", "SerializeObjectRefs",
            "MakeObjectRef", "IsAllDigits", "ResolveSceneObjectRef", "SetObjectEnabledState",
            "SetObjectsEnabledState", "GetObjectReferencePosition", "GetCurrentObjectName",
            "TryPlayAnimationClipNamed", "ResolveUITextTarget", "SetUITextLabel",
            "SetUITextEffects", "SetRigidbody2DSimulated", "DrawStdStringInput",
            "DrawObjectRefInput", "IsAudioClipPath", "DrawAudioClipInput",
            "DrawObjectRefListEditor", "IEnum_Start", "IEnum_Stop", "IEnum_Ensure",
            // ImGui
            //"ImGui::Text", "ImGui::TextUnformatted", "ImGui::TextWrapped", "ImGui::TextDisabled", "ImGui::TextColored",
            //"ImGui::Button", "ImGui::SmallButton", "ImGui::InvisibleButton", "ImGui::ArrowButton",
            //"ImGui::InputText", "ImGui::InputTextMultiline", "ImGui::InputInt", "ImGui::InputFloat", "ImGui::InputFloat2", "ImGui::InputFloat3",
            //"ImGui::SliderFloat", "ImGui::SliderInt", "ImGui::SliderFloat2", "ImGui::SliderFloat3",
            //"ImGui::Checkbox", "ImGui::RadioButton",
            //"ImGui::Combo", "ImGui::BeginCombo", "ImGui::EndCombo",
            //"ImGui::ColorEdit3", "ImGui::ColorEdit4", "ImGui::ColorPicker3", "ImGui::ColorPicker4",
            //"ImGui::BeginChild", "ImGui::EndChild",
            //"ImGui::BeginTabBar", "ImGui::EndTabBar", "ImGui::BeginTabItem", "ImGui::EndTabItem",
            //"ImGui::BeginMenuBar", "ImGui::EndMenuBar", "ImGui::BeginMenu", "ImGui::EndMenu", "ImGui::MenuItem",
            //"ImGui::BeginPopup", "ImGui::BeginPopupModal", "ImGui::EndPopup", "ImGui::OpenPopup", "ImGui::CloseCurrentPopup",
            //"ImGui::BeginTooltip", "ImGui::EndTooltip", "ImGui::SetTooltip",
            //"ImGui::BeginTable", "ImGui::EndTable", "ImGui::TableNextRow", "ImGui::TableNextColumn", "ImGui::TableSetupColumn",
            //"ImGui::CollapsingHeader", "ImGui::TreeNode", "ImGui::TreePop", "ImGui::TreeNodeEx",
            //"ImGui::Separator", "ImGui::SameLine", "ImGui::Spacing", "ImGui::NewLine", "ImGui::Indent", "ImGui::Unindent",
            //"ImGui::PushStyleColor", "ImGui::PopStyleColor", "ImGui::PushStyleVar", "ImGui::PopStyleVar",
            //"ImGui::PushItemWidth", "ImGui::PopItemWidth", "ImGui::SetNextItemWidth",
            //"ImGui::GetContentRegionAvail", "ImGui::GetWindowSize", "ImGui::GetWindowPos",
            //"ImGui::SetScrollHereY", "ImGui::GetScrollY", "ImGui::GetScrollMaxY",
            //"ImGui::IsItemHovered", "ImGui::IsItemClicked", "ImGui::IsItemActive", "ImGui::IsItemEdited",
            //"ImGui::IsWindowFocused", "ImGui::IsWindowHovered",
            //"ImGui::GetIO", "ImGui::GetStyle", "ImGui::GetDrawList",
            //"ImGui::SetNextWindowSize", "ImGui::SetNextWindowPos", "ImGui::SetNextWindowContentSize",
            //"ImGui::Begin", "ImGui::End",
            //"ImGui::Image", "ImGui::ImageButton",
            //"ImGui::ProgressBar", "ImGui::Bullet",
        };
        return std::vector<std::string>(std::begin(kBuiltins), std::end(kBuiltins));
    }

    static std::unordered_map<std::string, std::string> moduCppBuiltinSignatures() {
        return {
            {"Begin", "void Begin()"},
            {"TickUpdate", "void TickUpdate(float deltaTime)"},
            {"Update", "void Update(float deltaTime)"},
            {"Spec", "void Spec()"},
            {"TestEditor", "void TestEditor()"},
            {"RenderEditorWindow", "void RenderEditorWindow()"},
            {"ExitRenderEditorWindow", "void ExitRenderEditorWindow()"},
            {"Script_OnInspector", "void Script_OnInspector()"},
            {"OnCollideEnter", "OnCollideEnter(Col other)"},
            {"OnCollideHold", "OnCollideHold(Col other)"},
            {"OnCollideExit", "OnCollideExit(Col other)"},
            {"Calc", "Calc Name(value) to value"},
            {"AutoFields", "AutoFields(field, ...)"},
            {"each", "each(ref value in values)"}
        };
    }

    static bool isModuMakoSource(const std::string& text) {
        return text.find("using ModuMAKO;") != std::string::npos ||
               (text.find("script \"") != std::string::npos && text.find(": ModuNode;") != std::string::npos);
    }

    static std::vector<std::string> moduMakoBuiltins() {
        return {
            "ModuMAKO", "ModuEngine", "ModuInput", "ModuNode", "SceneObj",
            "Vector2", "Vector3", "Vector4", "Color", "List", "Array",
            "MovementSettings", "MovementState", "MovementDebug",
            "StandaloneMovementSettings", "StandaloneMovementState", "StandaloneMovementDebug",
            "Begin", "TickUpdate", "Update", "Spec", "TestEditor",
            "OnCollideEnter", "OnCollideHold", "OnCollideExit",
            "EnsureRigidbody", "EnsureCapsuleCollider", "TickStandaloneMovement",
            "AddRigidbodyImpulse", "SetPosition", "AddLog", "Type", "obj",
            "Input.WASDMovement", "Input.ButtonHeld", "Input.ButtonDown", "Input.IsKeyDown",
            "push", "pop", "first", "last", "has", "len", "range", "assert"
        };
    }

    static std::unordered_map<std::string, std::string> moduMakoBuiltinSignatures() {
        return {
            {"Begin", "Begin()"},
            {"TickUpdate", "TickUpdate(dt)"},
            {"Update", "Update(dt)"},
            {"EnsureRigidbody", "EnsureRigidbody(dynamic, useGravity)"},
            {"EnsureCapsuleCollider", "EnsureCapsuleCollider(height, radius)"},
            {"TickStandaloneMovement", "TickStandaloneMovement(state, settings, dt, debug)"},
            {"AddRigidbodyImpulse", "AddRigidbodyImpulse(impulse)"},
            {"SetPosition", "SetPosition(position)"},
            {"AddLog", "AddLog(message, type)"},
            {"WASDMovement", "Input.WASDMovement()"},
            {"ButtonHeld", "Input.ButtonHeld(action)"},
            {"ButtonDown", "Input.ButtonDown(action)"},
            {"IsKeyDown", "Input.IsKeyDown(key)"},
            {"push", "push(collection, value)"},
            {"pop", "pop(collection)"},
            {"first", "first(collection)"},
            {"last", "last(collection)"},
            {"has", "has(collection, value)"},
            {"len", "len(collection)"},
            {"range", "range(stop) or range(start, stop, step)"},
            {"assert", "assert(condition, message)"}
        };
    }

    static std::vector<std::string> extractMakoVariables(const std::string& text) {
        std::unordered_set<std::string> unique;
        static const std::regex assignment(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?::[^=]+)?=)");
        static const std::regex loop(R"(^\s*for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\b)");
        std::istringstream input(text);
        std::string line;
        std::smatch match;
        while (std::getline(input, line)) {
            if (std::regex_search(line, match, assignment) || std::regex_search(line, match, loop)) {
                if (match.size() > 1 && match[1].str().size() >= 2) unique.insert(match[1].str());
            }
        }
        std::vector<std::string> out(unique.begin(), unique.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    static std::vector<std::string> extractFunctionIdentifiers(const std::string& text,
                                                               const std::unordered_set<std::string>& keywords) {
        static const std::unordered_set<std::string> kSkip = {
            "if", "for", "while", "switch", "catch", "return", "sizeof", "alignof", "defined", "layout"
        };
        std::unordered_set<std::string> unique;
        size_t i = 0;
        while (i < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (!isIdentifierStart(c)) {
                ++i;
                continue;
            }
            size_t start = i++;
            while (i < text.size() && isIdentifierBody(static_cast<unsigned char>(text[i]))) {
                ++i;
            }
            std::string token = text.substr(start, i - start);
            size_t j = i;
            while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) {
                ++j;
            }
            if (j < text.size() && text[j] == '(' &&
                keywords.find(token) == keywords.end() &&
                kSkip.find(token) == kSkip.end()) {
                unique.insert(std::move(token));
            }
        }
        std::vector<std::string> out(unique.begin(), unique.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    static std::vector<std::string> buildSymbolList(const std::string& text) {
        std::vector<std::string> symbols;
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) {
            std::string trimmed = trimLeft(line);
            if (trimmed.empty()) continue;
            if (trimmed.rfind("//", 0) == 0) continue;

            auto captureToken = [&](const std::string& prefix) -> bool {
                if (trimmed.rfind(prefix, 0) != 0) return false;
                size_t start = prefix.size();
                while (start < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[start]))) {
                    ++start;
                }
                size_t end = start;
                while (end < trimmed.size() &&
                       (std::isalnum(static_cast<unsigned char>(trimmed[end])) || trimmed[end] == '_' || trimmed[end] == ':')) {
                    ++end;
                }
                if (end > start) {
                    symbols.emplace_back(trimmed.substr(0, end));
                    return true;
                }
                return false;
            };

            if (captureToken("class ") || captureToken("struct ") || captureToken("enum ") || captureToken("namespace ")) {
                continue;
            }

            if (trimmed.find('(') != std::string::npos && trimmed.find(')') != std::string::npos &&
                (trimmed.find('{') != std::string::npos || trimmed.back() == ';')) {
                static const char* kSkip[] = {"if", "for", "while", "switch", "catch"};
                bool skip = false;
                for (const char* keyword : kSkip) {
                    if (trimmed.rfind(keyword, 0) == 0) {
                        skip = true;
                        break;
                    }
                }
                if (skip) continue;
                size_t paren = trimmed.find('(');
                if (paren != std::string::npos && paren > 0) {
                    size_t end = paren;
                    while (end > 0 && std::isspace(static_cast<unsigned char>(trimmed[end - 1]))) {
                        --end;
                    }
                    size_t start = end;
                    while (start > 0 &&
                           (std::isalnum(static_cast<unsigned char>(trimmed[start - 1])) || trimmed[start - 1] == '_' ||
                            trimmed[start - 1] == ':')) {
                        --start;
                    }
                    if (end > start) {
                        symbols.emplace_back(trimmed.substr(start, paren - start));
                    }
                }
            }
        }
        return symbols;
    }

    static std::unordered_map<std::string, std::string> extractFunctionSignatures(
        const std::string& text,
        const std::unordered_set<std::string>& keywords) {
        static const std::unordered_set<std::string> kSkip = {
            "if", "for", "while", "switch", "catch", "return", "sizeof", "alignof", "defined", "layout"
        };

        std::unordered_map<std::string, std::string> signatures;
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) {
            std::string trimmed = trimLeft(line);
            if (trimmed.empty()) continue;
            if (trimmed.rfind("//", 0) == 0) continue;
            size_t openParen = trimmed.find('(');
            size_t closeParen = trimmed.find(')', openParen == std::string::npos ? 0 : openParen + 1);
            if (openParen == std::string::npos || closeParen == std::string::npos) continue;
            size_t nameEnd = openParen;
            while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(trimmed[nameEnd - 1]))) {
                --nameEnd;
            }
            size_t nameStart = nameEnd;
            while (nameStart > 0) {
                unsigned char c = static_cast<unsigned char>(trimmed[nameStart - 1]);
                if (isIdentifierBody(c) || trimmed[nameStart - 1] == ':' || trimmed[nameStart - 1] == '~') {
                    --nameStart;
                    continue;
                }
                break;
            }
            if (nameStart >= nameEnd) continue;
            std::string name = trimmed.substr(nameStart, nameEnd - nameStart);
            if (keywords.find(name) != keywords.end() || kSkip.find(name) != kSkip.end()) continue;
            size_t sigEnd = trimmed.find('{');
            if (sigEnd == std::string::npos) {
                sigEnd = trimmed.find(';');
            }
            std::string signature = (sigEnd == std::string::npos) ? trimmed : trimmed.substr(0, sigEnd);
            while (!signature.empty() && std::isspace(static_cast<unsigned char>(signature.back()))) {
                signature.pop_back();
            }
            if (signature.empty()) continue;
            signatures.emplace(name, signature);
        }
        return signatures;
    }

    static std::vector<std::string> extractIdentifiers(const std::string& text,
                                                       const std::unordered_set<std::string>& keywords) {
        std::unordered_set<std::string> unique;
        std::string token;
        token.reserve(64);
        auto flushToken = [&]() {
            if (token.size() >= 2 && keywords.find(token) != keywords.end()) {
                token.clear();
                return;
            }
            if (token.size() >= 2) {
                unique.insert(token);
            }
            token.clear();
        };
        for (char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                token.push_back(c);
            } else if (!token.empty()) {
                flushToken();
            }
        }
        if (!token.empty()) {
            flushToken();
        }
        std::vector<std::string> out(unique.begin(), unique.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    static void addContextSymbol(std::unordered_map<std::string, std::string>& symbols,
                                 const std::string& name,
                                 const fs::path& sourcePath,
                                 const char* kind) {
        if (name.size() < 2 || !isIdentifierStart(static_cast<unsigned char>(name[0]))) return;
        for (char c : name) {
            if (!isIdentifierBody(static_cast<unsigned char>(c)) && c != '.' && c != ':' && c != '-') {
                return;
            }
        }
        symbols.emplace(name, std::string(kind) + " from " + sourcePath.filename().string());
    }

    static std::unordered_map<std::string, std::string> extractContextSymbols(const fs::path& sourcePath,
                                                                              const std::string& text) {
        static const std::unordered_set<std::string> kIgnored = {
            "true", "false", "null", "none", "type", "name", "id", "path", "file", "enabled",
            "position", "rotation", "scale", "x", "y", "z", "w"
        };
        std::unordered_map<std::string, std::string> symbols;
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) {
            std::string trimmed = trimCopy(line);
            if (trimmed.empty() || trimmed.rfind("#", 0) == 0 || trimmed.rfind("//", 0) == 0) continue;

            size_t sep = trimmed.find_first_of(":=");
            if (sep != std::string::npos) {
                std::string key = trimCopy(trimmed.substr(0, sep));
                if (!key.empty() && key.front() == '"' && key.back() == '"' && key.size() > 1) {
                    key = key.substr(1, key.size() - 2);
                }
                if (kIgnored.find(toLowerCopy(key)) == kIgnored.end()) {
                    addContextSymbol(symbols, key, sourcePath, ".ctx key");
                }
            }

            std::string token;
            token.reserve(64);
            auto flushToken = [&]() {
                if (token.size() >= 2 && kIgnored.find(toLowerCopy(token)) == kIgnored.end()) {
                    addContextSymbol(symbols, token, sourcePath, ".ctx symbol");
                }
                token.clear();
            };
            for (char c : trimmed) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':' || c == '-') {
                    token.push_back(c);
                } else if (!token.empty()) {
                    flushToken();
                }
            }
            if (!token.empty()) {
                flushToken();
            }
        }
        return symbols;
    }
}

const std::unordered_set<std::string>& ScriptLanguageService::keywordsForLanguage(
    ScriptLanguageServiceLanguage language) {
    static const std::unordered_set<std::string> kCppKeywords = {
        "auto", "bool", "break", "case", "catch", "char", "class", "const", "constexpr", "continue",
        "default", "delete", "do", "double", "else", "enum", "explicit", "extern", "false", "float",
        "for", "friend", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
        "operator", "private", "protected", "public", "return", "short", "signed", "sizeof", "static",
        "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "while"
    };
    static const std::unordered_set<std::string> kCKeywords = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
        "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
        "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"
    };
    static const std::unordered_set<std::string> kGlslKeywords = {
        "attribute", "const", "uniform", "varying", "layout", "centroid", "flat", "smooth", "noperspective",
        "break", "continue", "do", "for", "while", "switch", "case", "default", "if", "else", "in", "out",
        "inout", "float", "double", "int", "void", "bool", "true", "false", "invariant", "discard", "return",
        "mat2", "mat3", "mat4", "dmat2", "dmat3", "dmat4", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
        "bvec2", "bvec3", "bvec4", "sampler1D", "sampler2D", "sampler3D", "samplerCube", "sampler2DShadow",
        "samplerCubeShadow", "sampler2DArray", "sampler2DArrayShadow", "isampler2D", "usampler2D", "struct",
        "precision", "highp", "mediump", "lowp", "uint"
    };
    static const std::unordered_set<std::string> kLuaKeywords = {
        "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto",
        "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"
    };
    static const std::unordered_set<std::string> kModuCppKeywords = [] {
        std::unordered_set<std::string> keywords = {
            "add", "alignof", "and", "and_eq", "auto", "bitand", "bitor", "bool", "break", "case",
            "catch", "char", "class", "compl", "concept", "const", "const_cast", "constexpr", "continue",
            "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
            "dynamic_cast", "each", "else", "enum", "explicit", "extern", "false", "float", "for",
            "friend", "goto", "if", "in", "inline", "int", "long", "mutable", "namespace", "new",
            "noexcept", "not", "not_eq", "null", "nullptr", "operator", "or", "or_eq", "private",
            "protected", "public", "ref", "register", "reinterpret_cast", "requires", "return", "short",
            "signed", "sizeof", "static", "static_cast", "struct", "SubScript", "switch", "template",
            "this", "throw", "to", "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
            "using", "virtual", "void", "volatile", "while", "xor", "xor_eq"
        };
        for (const std::string& localized : ModuCPPLang::LocalizedVocabulary()) {
            keywords.insert(localized);
        }
        return keywords;
    }();
    static const std::unordered_set<std::string> kMakoKeywords = {
        "and", "as", "break", "continue", "else", "false", "for", "if", "in", "none",
        "not", "or", "return", "script", "true", "using", "while"
    };
    static const std::unordered_set<std::string> kPlainTextKeywords;

    switch (language) {
        case ScriptLanguageServiceLanguage::PlainText:
            return kPlainTextKeywords;
        case ScriptLanguageServiceLanguage::C:
            return kCKeywords;
        case ScriptLanguageServiceLanguage::GLSL:
        case ScriptLanguageServiceLanguage::HLSL:
            return kGlslKeywords;
        case ScriptLanguageServiceLanguage::Lua:
            return kLuaKeywords;
        case ScriptLanguageServiceLanguage::ModuCPP:
            return kModuCppKeywords;
        case ScriptLanguageServiceLanguage::Mako:
            return kMakoKeywords;
        case ScriptLanguageServiceLanguage::Cpp:
        default:
            return kCppKeywords;
    }
}

ScriptLanguageServiceLanguage ScriptLanguageService::detectLanguage(const fs::path& path) {
    const std::string ext = extensionLower(path);
    if (ext == ".c") return ScriptLanguageServiceLanguage::C;
    if (ext == ".glsl" || ext == ".vert" || ext == ".frag") return ScriptLanguageServiceLanguage::GLSL;
    if (ext == ".hlsl" || ext == ".shader") return ScriptLanguageServiceLanguage::HLSL;
    if (ext == ".lua") return ScriptLanguageServiceLanguage::Lua;
    if (ext == ".moducpp") return ScriptLanguageServiceLanguage::ModuCPP;
    if (ext == ".mko" || ext == ".modumako") return ScriptLanguageServiceLanguage::Mako;
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c++" ||
        ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx" || ext == ".cs") {
        return ScriptLanguageServiceLanguage::Cpp;
    }
    return ScriptLanguageServiceLanguage::PlainText;
}

bool ScriptLanguageService::isCompilableScriptPath(const fs::path& path) {
    const std::string ext = extensionLower(path);
    return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" ||
           ext == ".moducpp" || ext == ".mko" || ext == ".modumako" ||
           ext == ".cs" || ext == ".csproj";
}

static std::vector<std::string> extractQualifiedCallIdentifiers(const std::string& text) {
    std::unordered_set<std::string> unique;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '"' || text[i] == '\'') {
            const char delim = text[i++];
            while (i < text.size() && text[i] != delim) {
                if (text[i] == '\\') ++i;
                ++i;
            }
            ++i;
            continue;
        }
        if (i + 1 < text.size() && text[i] == '/' && text[i+1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (i + 1 < text.size() && text[i] == '/' && text[i+1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i+1] == '/')) ++i;
            i += 2;
            continue;
        }
        if (!isIdentifierStart(static_cast<unsigned char>(text[i]))) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < text.size()) {
            if (isIdentifierBody(static_cast<unsigned char>(text[i]))) {
                ++i;
            } else if (i + 1 < text.size() && text[i] == ':' && text[i+1] == ':') {
                i += 2;
            } else {
                break;
            }
        }
        const std::string token = text.substr(start, i - start);
        if (token.find("::") == std::string::npos) continue;
        size_t j = i;
        while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
        if (j < text.size() && text[j] == '(') {
            unique.insert(token);
        }
    }
    std::vector<std::string> out(unique.begin(), unique.end());
    std::sort(out.begin(), out.end());
    return out;
}

ScriptLanguageServiceProjectData ScriptLanguageService::scanProjectFiles(const fs::path& projectRoot,
                                                                         const fs::path& assetsPath,
                                                                         const ScriptBuildConfig* config) {
    ScriptLanguageServiceProjectData result;

    static const std::unordered_set<std::string> kValidExt = {
        ".cpp", ".cc", ".cxx", ".c", ".moducpp", ".mko", ".modumako", ".hpp", ".h", ".inl",
        ".glsl", ".vert", ".frag", ".hlsl", ".shader", ".lua"
    };
    static const std::unordered_set<std::string> kContextExt = {
        ".ctx"
    };

    std::vector<fs::path> projectRoots;
    std::vector<fs::path> includeRoots;
    std::vector<fs::path> contextRoots;

    if (config != nullptr) {
        projectRoots.push_back(config->scriptsDir);
        for (const fs::path& includeDir : config->includeDirs) {
            includeRoots.push_back(includeDir);
        }
    } else {
        projectRoots.push_back(assetsPath / "Scripts");
    }
    projectRoots.push_back(projectRoot / "Scripts");
    projectRoots.push_back(projectRoot / "Assets" / "Scripts");
    projectRoots.push_back(projectRoot / "include");
    projectRoots.push_back(projectRoot / "src");
    projectRoots.push_back(assetsPath / "Shaders");
    projectRoots.push_back(projectRoot / "Shaders");
    contextRoots.push_back(projectRoot);
    contextRoots.push_back(assetsPath);

    std::unordered_set<std::string> uniqueSymbols;
    std::unordered_set<std::string> uniqueCompletions;
    std::unordered_set<std::string> uniquePaths;

    size_t includeFilesScanned = 0;
    for (const auto& root : includeRoots) {
        std::error_code ec;
        fs::path resolvedRoot = root;
        if (!resolvedRoot.empty() && !resolvedRoot.is_absolute()) {
            resolvedRoot = projectRoot / resolvedRoot;
        }
        if (resolvedRoot.empty() || !fs::exists(resolvedRoot, ec) || !fs::is_directory(resolvedRoot, ec)) continue;

        for (auto it = fs::recursive_directory_iterator(resolvedRoot, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (includeFilesScanned >= kMaxIncludeFiles) break;
            if (it->is_directory()) {
                const std::string dirName = it->path().filename().string();
                if (dirName == ".git" || dirName == "build" || dirName == "Build-Android" || dirName == "Build-ASAN" || dirName == "ThirdParty") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            const std::string ext = extensionLower(it->path());
            if (ext != ".h" && ext != ".hpp" && ext != ".inl") continue;

            std::error_code sec;
            const auto fileSize = fs::file_size(it->path(), sec);
            if (sec || fileSize > kMaxIncludeFileBytes) continue;

            std::ifstream file(it->path());
            if (!file.is_open()) continue;
            std::stringstream buffer;
            buffer << file.rdbuf();
            const std::string text = buffer.str();

            for (const auto& qualified : extractQualifiedCallIdentifiers(text)) {
                uniqueCompletions.insert(qualified);
            }
            for (const auto& [name, signature] : extractFunctionSignatures(text, keywordsForLanguage(ScriptLanguageServiceLanguage::Cpp))) {
                result.functionSignatures.emplace(name, signature);
            }
            ++includeFilesScanned;
        }
    }

    // Scan Modularity context metadata separately. .ctx files are not parsed as code.
    for (const auto& root : contextRoots) {
        std::error_code ec;
        fs::path resolvedRoot = root;
        if (!resolvedRoot.empty() && !resolvedRoot.is_absolute()) {
            resolvedRoot = projectRoot / resolvedRoot;
        }
        if (resolvedRoot.empty() || !fs::exists(resolvedRoot, ec) || !fs::is_directory(resolvedRoot, ec)) continue;

        for (auto it = fs::recursive_directory_iterator(resolvedRoot, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                result.scanWarnings.push_back("Stopped scanning context metadata in " + resolvedRoot.string() + ": " + ec.message());
                break;
            }
            if (it->is_directory()) {
                const std::string dirName = it->path().filename().string();
                if (dirName == ".git" || dirName == "build" || dirName == "Build-Android" || dirName == "Build-ASAN" || dirName == "cmake-build-debug" ||
                    dirName == "cmake-build-release" || dirName == "ThirdParty" || dirName == "Library" ||
                    dirName == "Cache") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (kContextExt.find(extensionLower(it->path())) == kContextExt.end()) continue;
            fs::path normalized = it->path().lexically_normal();
            std::string key = std::string("ctx:") + normalized.string();
            if (!uniquePaths.insert(key).second) continue;
            result.contextFiles.push_back(normalized);
        }
    }

    // Scan project files fully
    for (const auto& root : projectRoots) {
        std::error_code ec;
        fs::path resolvedRoot = root;
        if (!resolvedRoot.empty() && !resolvedRoot.is_absolute()) {
            resolvedRoot = projectRoot / resolvedRoot;
        }
        if (resolvedRoot.empty() || !fs::exists(resolvedRoot, ec) || !fs::is_directory(resolvedRoot, ec)) {
            if (!resolvedRoot.empty()) {
                result.scanWarnings.push_back("Skipped missing script language scan root: " + resolvedRoot.string());
            }
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(resolvedRoot, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                result.scanWarnings.push_back("Stopped scanning " + resolvedRoot.string() + ": " + ec.message());
                break;
            }
            if (it->is_directory()) {
                const std::string dirName = it->path().filename().string();
                if (dirName == ".git" || dirName == "build" || dirName == "Build-Android" || dirName == "Build-ASAN" || dirName == "cmake-build-debug" ||
                    dirName == "cmake-build-release" || dirName == "ThirdParty") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            const std::string ext = extensionLower(it->path());
            if (kContextExt.find(ext) != kContextExt.end()) {
                fs::path normalized = it->path().lexically_normal();
                std::string key = std::string("ctx:") + normalized.string();
                if (!uniquePaths.insert(key).second) continue;
                result.contextFiles.push_back(normalized);
                continue;
            }
            if (kValidExt.find(ext) == kValidExt.end()) continue;
            fs::path normalized = it->path().lexically_normal();
            std::string key = normalized.string();
            if (!uniquePaths.insert(key).second) continue;
            result.files.push_back(normalized);
        }
    }

    std::sort(result.files.begin(), result.files.end());
    std::sort(result.contextFiles.begin(), result.contextFiles.end());

    for (const auto& scriptPath : result.files) {
        std::ifstream file(scriptPath);
        if (!file.is_open()) {
            result.scanWarnings.push_back("Could not read script language source: " + scriptPath.string());
            continue;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        const ScriptLanguageServiceLanguage language = detectLanguage(scriptPath);
        const std::string text = language == ScriptLanguageServiceLanguage::ModuCPP
            ? ModuCPPLang::CanonicalizeAuto(buffer.str())
            : buffer.str();
        const auto& keywords = keywordsForLanguage(language);
        for (const auto& symbol : buildSymbolList(text)) {
            uniqueSymbols.insert(symbol);
            uniqueCompletions.insert(symbol);
        }
        for (const auto& identifier : extractIdentifiers(text, keywords)) uniqueCompletions.insert(identifier);
        for (const auto& function : extractFunctionIdentifiers(text, keywords)) uniqueCompletions.insert(function);
        for (const auto& define : extractDefineIdentifiers(text)) uniqueCompletions.insert(define);
        for (const auto& variable : extractVariableIdentifiers(text, language, keywords)) uniqueCompletions.insert(variable);
        for (const auto& field : extractAutoFieldIdentifiers(text)) uniqueCompletions.insert(field);
        for (const auto& importName : extractModuCppImports(text)) uniqueCompletions.insert(importName);
        for (const auto& qualified : extractQualifiedCallIdentifiers(text)) uniqueCompletions.insert(qualified);
        for (const auto& [name, signature] : extractFunctionSignatures(text, keywords)) {
            result.functionSignatures.emplace(name, signature);
        }
    }

    for (const auto& contextPath : result.contextFiles) {
        std::ifstream file(contextPath);
        if (!file.is_open()) {
            result.scanWarnings.push_back("Could not read Modularity context source: " + contextPath.string());
            continue;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();
        for (const auto& [name, detail] : extractContextSymbols(contextPath, text)) {
            uniqueSymbols.insert(name);
            uniqueCompletions.insert(name);
            result.symbolDetails.emplace(name, detail);
        }
    }

    for (const std::string& builtin : moduCppBuiltins()) uniqueCompletions.insert(builtin);
    for (const auto& [name, signature] : moduCppBuiltinSignatures()) result.functionSignatures.emplace(name, signature);

    result.projectSymbols.assign(uniqueSymbols.begin(), uniqueSymbols.end());
    std::sort(result.projectSymbols.begin(), result.projectSymbols.end());
    result.projectCompletions.assign(uniqueCompletions.begin(), uniqueCompletions.end());
    std::sort(result.projectCompletions.begin(), result.projectCompletions.end());
    return result;
}

ScriptLanguageServiceDocumentData ScriptLanguageService::analyzeDocument(const fs::path& path,
                                                                        const std::string& rawText) {
    ScriptLanguageServiceDocumentData result;
    result.language = detectLanguage(path);
    if (result.language == ScriptLanguageServiceLanguage::PlainText) {
        return result;
    }
    const std::string text = result.language == ScriptLanguageServiceLanguage::ModuCPP
        ? ModuCPPLang::CanonicalizeAuto(rawText)
        : rawText;
    const auto& keywords = keywordsForLanguage(result.language);
    result.isModuMako = result.language == ScriptLanguageServiceLanguage::Mako && isModuMakoSource(text);
    result.identifiers = extractIdentifiers(text, keywords);
    result.functions = extractFunctionIdentifiers(text, keywords);
    result.defines = extractDefineIdentifiers(text);
    result.symbols = buildSymbolList(text);
    result.variables = extractVariableIdentifiers(text, result.language, keywords);
    if (result.isModuMako) result.variables = extractMakoVariables(text);
    result.moducppImports = result.language == ScriptLanguageServiceLanguage::ModuCPP
        ? extractModuCppImports(text)
        : std::vector<std::string>{};
    result.moducppInspectorFields = result.language == ScriptLanguageServiceLanguage::ModuCPP
        ? extractAutoFieldIdentifiers(text)
        : std::vector<std::string>{};
    if (result.language == ScriptLanguageServiceLanguage::ModuCPP) {
        std::unordered_set<std::string> completions;
        for (const auto& entry : moduCppBuiltins()) completions.insert(entry);
        for (const auto& entry : result.moducppImports) completions.insert(entry);
        for (const auto& entry : result.moducppInspectorFields) completions.insert(entry);
        result.moducppCompletions.assign(completions.begin(), completions.end());
        std::sort(result.moducppCompletions.begin(), result.moducppCompletions.end());
    }
    if (result.isModuMako) {
        std::unordered_set<std::string> completions(result.moducppCompletions.begin(), result.moducppCompletions.end());
        for (const auto& entry : moduMakoBuiltins()) completions.insert(entry);
        result.moducppCompletions.assign(completions.begin(), completions.end());
        std::sort(result.moducppCompletions.begin(), result.moducppCompletions.end());
    }
    result.functionSignatures = extractFunctionSignatures(text, keywords);
    if (result.language == ScriptLanguageServiceLanguage::ModuCPP) {
        for (const auto& [name, signature] : moduCppBuiltinSignatures()) {
            result.functionSignatures.emplace(name, signature);
        }
    }
    if (result.isModuMako) {
        for (const auto& [name, signature] : moduMakoBuiltinSignatures()) {
            result.functionSignatures.insert_or_assign(name, signature);
        }
    }
    return result;
}

std::vector<std::string> ScriptLanguageService::buildQualifiedCompletionPool(
    const std::vector<std::string>& pool,
    const std::string& qualifier) {

    if (qualifier.empty()) return pool;

    const std::string colonPrefix = qualifier + "::";
    const std::string dotPrefix   = qualifier + ".";

    std::vector<std::string> filtered;
    for (const auto& entry : pool) {
        std::string tail;
        if (entry.rfind(colonPrefix, 0) == 0) {
            tail = entry.substr(colonPrefix.size());
        } else if (entry.rfind(dotPrefix, 0) == 0) {
            tail = entry.substr(dotPrefix.size());
        }
        if (!tail.empty() && tail.find(':') == std::string::npos && tail.find('.') == std::string::npos) {
            filtered.push_back(tail);
        }
    }

    return filtered;
}

std::vector<std::string> ScriptLanguageService::buildCompletionList(const std::vector<std::string>& pool,
                                                                    const std::string& prefix,
                                                                    size_t limit) {
    std::vector<std::string> matches;
    std::unordered_set<std::string> seen;

    if (prefix.empty()) {
        for (const auto& entry : pool) {
            if (seen.insert(entry).second) {
                matches.push_back(entry);
            }
            if (matches.size() >= limit) break;
        }
        return matches;
    }

    for (const auto& entry : pool) {
        if (entry.rfind(prefix, 0) == 0) {
            if (seen.insert(entry).second) {
                matches.push_back(entry);
            }
            if (matches.size() >= limit) break;
        }
    }
    if (matches.size() >= limit) return matches;
    const std::string lowerPrefix = toLowerCopy(prefix);
    for (const auto& entry : pool) {
        if (toLowerCopy(entry).rfind(lowerPrefix, 0) == 0) {
            if (seen.insert(entry).second) {
                matches.push_back(entry);
            }
            if (matches.size() >= limit) break;
        }
    }
    return matches;
}

std::vector<std::string> ScriptLanguageService::splitSignatureParameters(const std::string& signature) {
    std::vector<std::string> params;
    const size_t open = signature.find('(');
    const size_t close = signature.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open + 1) {
        return params;
    }
    std::string body = signature.substr(open + 1, close - open - 1);
    std::string current;
    int nested = 0;
    for (char c : body) {
        if (c == '<' || c == '(' || c == '[') {
            ++nested;
        } else if (c == '>' || c == ')' || c == ']') {
            nested = std::max(0, nested - 1);
        }
        if (c == ',' && nested == 0) {
            std::string trimmed = trimLeft(current);
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
                trimmed.pop_back();
            }
            if (!trimmed.empty()) {
                params.push_back(trimmed);
            }
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    std::string trimmed = trimLeft(current);
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    if (!trimmed.empty()) {
        params.push_back(trimmed);
    }
    return params;
}

ScriptLanguageServiceFunctionCallContext ScriptLanguageService::detectFunctionCallContext(const std::string& currentLine,
                                                                                          int cursorColumn) {
    ScriptLanguageServiceFunctionCallContext ctx;
    if (currentLine.empty()) return ctx;
    const int clampedColumn = std::clamp(cursorColumn, 0, static_cast<int>(currentLine.size()));
    int depth = 0;
    for (int i = clampedColumn - 1; i >= 0; --i) {
        char c = currentLine[static_cast<size_t>(i)];
        if (c == ')') {
            ++depth;
            continue;
        }
        if (c == '(') {
            if (depth > 0) {
                --depth;
                continue;
            }
            int end = i - 1;
            while (end >= 0 && std::isspace(static_cast<unsigned char>(currentLine[static_cast<size_t>(end)]))) {
                --end;
            }
            int start = end;
            while (start >= 0) {
                unsigned char ch = static_cast<unsigned char>(currentLine[static_cast<size_t>(start)]);
                char raw = currentLine[static_cast<size_t>(start)];
                if (isIdentifierBody(ch) || raw == ':' || raw == '.' || raw == '~') {
                    --start;
                    continue;
                }
                break;
            }
            if (end <= start) return ctx;
            ctx.functionName = currentLine.substr(static_cast<size_t>(start + 1),
                static_cast<size_t>(end - start));
            if (ctx.functionName.empty()) return ctx;
            int nested = 0;
            int argumentIndex = 0;
            for (int k = i + 1; k < clampedColumn; ++k) {
                char argChar = currentLine[static_cast<size_t>(k)];
                if (argChar == '(') {
                    ++nested;
                } else if (argChar == ')') {
                    nested = std::max(0, nested - 1);
                } else if (argChar == ',' && nested == 0) {
                    ++argumentIndex;
                }
            }
            ctx.valid = true;
            ctx.activeParameter = std::max(0, argumentIndex);
            return ctx;
        }
        if ((c == ';' || c == '{' || c == '}') && depth == 0) {
            break;
        }
    }
    return ctx;
}
