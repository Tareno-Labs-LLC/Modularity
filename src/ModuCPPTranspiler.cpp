#include "ModuCPPTranspiler.h"
#include "ModuCPPLanguagePack.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
namespace {
    enum class FieldVisibility {
        Public,
        Private
    };
    enum class FieldKind {
        Float,
        Int,
        Bool,
        Vec3,
        String,
        ObjectRef,
        ObjectList,
        SpriteRef,
        DialogueLines,
        Custom
    };
    struct FieldSpec {
        FieldVisibility visibility = FieldVisibility::Private;
        FieldKind kind = FieldKind::Custom;
        std::string rawType;
        std::string baseType;
        std::vector<std::string> arrayDimensions;
        std::string name;
        std::string initializer;
        std::optional<std::string> inspectorHeader;
        std::optional<std::string> sliderMinExpr;
        std::optional<std::string> sliderMaxExpr;
        std::optional<std::string> soundSetLabel;
        std::optional<std::string> rangeMinExpr;
        std::optional<std::string> rangeMaxExpr;
        std::optional<std::string> stepExpr;
        bool hasObjectRefAttribute = false;
        bool hasObjectListAttribute = false;
        bool hasDialogueLinesAttribute = false;
        bool hasSliderAttribute = false;
        bool hasClipGridPairAttribute = false;
        bool hasSeparatorAttribute = false;
        bool hasInspectorMetadata = false;
        bool persist = false;
    };
    struct SubScriptSpec {
        std::string name;
        std::string rawDefinition;
        std::vector<FieldSpec> fields;
    };
    struct MethodSpec {
        std::string returnType = "void";
        std::string name;
        std::string originalParams;
        std::string transpiledParams;
        std::string body;
        int bodySourceLine = 0;
        bool isStatic = false;
        bool hasContext = false;
        bool contextIsPointer = false;
        std::string contextParamName;
        bool hasManualPrelude = false;
        bool autoInjectedContext = false;
        bool autoInjectedDeltaTime = false;
        bool hasDeltaTimeParam = false;
        bool isCalc = false;
        std::string collisionHoldDuration;
        std::string deltaTimeParamName = "dt";
    };
    struct ClassSpec {
        std::string name;
        std::vector<std::string> includeDirectives;
        std::vector<std::string> usingDirectives;
        std::vector<FieldSpec> fields;
        std::vector<SubScriptSpec> subScripts;
        std::vector<MethodSpec> methods;
        std::string inspectorBlock;
        std::string passthroughCode;
    };
    struct ModuClassMatch {
        std::string name;
        size_t position = std::string::npos;
        size_t end = std::string::npos;
    };
    size_t findTopLevelChar(const std::string& text, char needle);
    bool isVoidReturnType(const std::string& returnType);
    std::string trimCopy(const std::string& value) {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) ++start;
        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
        return value.substr(start, end - start);
    }
    bool isIdentifierStartChar(char c) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalpha(uc) != 0 || c == '_' || uc >= 0x80;
    }
    bool isIdentifierChar(char c) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) != 0 || c == '_' || uc >= 0x80;
    }
    size_t skipWhitespace(const std::string& text, size_t pos) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) ++pos;
        return pos;
    }
    std::optional<ModuClassMatch> findModuClassDeclaration(const std::string& text) {
        size_t cursor = 0;
        while (cursor < text.size()) {
            const size_t publicPos = text.find("public", cursor);
            if (publicPos == std::string::npos) return std::nullopt;
            const bool publicHasLeftBoundary  = publicPos == 0 || !isIdentifierChar(text[publicPos - 1]);
            const size_t publicEnd            = publicPos + 6;
            const bool publicHasRightBoundary = publicEnd >= text.size() || !isIdentifierChar(text[publicEnd]);
            if (!publicHasLeftBoundary || !publicHasRightBoundary) {cursor = publicEnd; continue;}
            size_t pos = skipWhitespace(text, publicEnd);
            if (text.compare(pos, 5, "class") != 0 ||
                (pos + 5 < text.size() && isIdentifierChar(text[pos + 5]))) {
                cursor = publicEnd; continue;
            }
            pos = skipWhitespace(text, pos + 5);
            if (pos >= text.size() || !isIdentifierStartChar(text[pos])) {cursor = publicEnd; continue;}
            const size_t nameStart = pos++;
            while (pos < text.size() && isIdentifierChar(text[pos])) ++pos;
            const std::string className = text.substr(nameStart, pos - nameStart);
            pos = skipWhitespace(text, pos);
            if (pos >= text.size() || text[pos] != ':') {cursor = publicEnd; continue;}
            pos = skipWhitespace(text, pos + 1);
            const char* bases[] = {"ModuBehaviour", "ModuNode"};
            for (const char* base : bases) {
                const size_t baseLen = std::strlen(base);
                if (text.compare(pos, baseLen, base) == 0 &&
                    (pos + baseLen >= text.size() || !isIdentifierChar(text[pos + baseLen]))) {
                    return ModuClassMatch{className, publicPos, pos + baseLen};
                }
            }
            cursor = publicEnd;
        }
        return std::nullopt;
    }
    size_t lineStartFromOffset(const std::string& text, size_t offset) {
        if (text.empty()) return 0;
        const size_t clamped = std::min(offset, text.size());
        const size_t lineBreak = text.rfind('\n', clamped == 0 ? 0 : clamped - 1);
        return lineBreak == std::string::npos ? 0 : lineBreak + 1;
    }
    size_t lineEndFromOffset(const std::string& text, size_t offset) {
        const size_t clamped = std::min(offset, text.size());
        const size_t lineBreak = text.find('\n', clamped);
        return lineBreak == std::string::npos ? text.size() : lineBreak;
    }
    size_t firstNonWhitespaceFrom(const std::string& text, size_t offset) {
        size_t pos = std::min(offset, text.size());
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) ++pos;
        return pos;
    }
    size_t previousStatementEnd(const std::string& text, size_t offset) {
        size_t pos = std::min(offset, text.size());
        while (pos > 0) {
            const unsigned char c = static_cast<unsigned char>(text[pos - 1]);
            if (c == '\n') break;
            if (std::isspace(c) == 0) return pos;
            --pos;
        }
        const size_t lineStart = lineStartFromOffset(text, offset);
        const size_t lineEnd = lineEndFromOffset(text, lineStart);
        size_t linePos = lineEnd;
        while (linePos > lineStart) {
            const unsigned char c = static_cast<unsigned char>(text[linePos - 1]);
            if (std::isspace(c) == 0) return linePos;
            --linePos;
        }
        return firstNonWhitespaceFrom(text, lineStart);
    }
    bool startsWithAccessModifier(const std::string& text, size_t offset) {
        if (offset >= text.size()) return false;
        return text.compare(offset, 7, "public ") == 0  ||
               text.compare(offset, 8, "private ") == 0 ||
               text.compare(offset, 7, "public:") == 0  ||
               text.compare(offset, 8, "private:") == 0;
    }
    std::optional<size_t> findSecondAccessModifierBoundary(const std::string& text) {
        bool foundAccessModifier = false;
        size_t lineStart = 0;
        while (lineStart <= text.size()) {
            const size_t lineFirstToken = firstNonWhitespaceFrom(text, lineStart);
            if (startsWithAccessModifier(text, lineFirstToken)) {
                if (foundAccessModifier) return previousStatementEnd(text, lineStart);
                foundAccessModifier = true;
            }
            const size_t lineBreak = text.find('\n', lineStart);
            if (lineBreak == std::string::npos) break;
            lineStart = lineBreak + 1;
        }
        return std::nullopt;
    }
    int lineNumberForOffset(const std::string& text, size_t offset) {
        const size_t clamped = std::min(offset, text.size());
        int line = 1;
        for (size_t i = 0; i < clamped; ++i) {if (text[i] == '\n') ++line;}
        return line;
    }
    int columnNumberForOffset(const std::string& text, size_t offset) {
        const size_t clamped = std::min(offset, text.size());
        const size_t lineStart = lineStartFromOffset(text, clamped);
        return static_cast<int>(clamped - lineStart) + 1;
    }
    std::string formatLocatedParseError(const std::string& message, const std::string& sourceText, size_t offset) {
        const size_t clamped = std::min(offset, sourceText.size());
        const int line = lineNumberForOffset(sourceText, clamped);
        const int column = columnNumberForOffset(sourceText, clamped);
        std::ostringstream out;
        out << message << " at line " << line << ", column " << column << ".";
        return out.str();
    }
    std::optional<size_t> findLikelyMissingSemicolonInFieldDecl(const std::string& fieldDecl) {
        if (std::optional<size_t> boundary = findSecondAccessModifierBoundary(fieldDecl)) return boundary;
        return std::nullopt;
    }
    std::optional<size_t> findLikelyMissingSemicolonBeforeMethod(const std::string& declaration) {
        if (std::optional<size_t> boundary = findSecondAccessModifierBoundary(declaration)) return boundary;
        const size_t openParenPos = findTopLevelChar(declaration, '(');
        if (openParenPos != std::string::npos) {
            const size_t methodLineStart = lineStartFromOffset(declaration, openParenPos);
            const std::string beforeMethodLine = trimCopy(declaration.substr(0, methodLineStart));
            if (!beforeMethodLine.empty()) {return previousStatementEnd(declaration, methodLineStart);}
        }
        const size_t eqPos = findTopLevelChar(declaration, '=');
        if (eqPos == std::string::npos || openParenPos == std::string::npos || eqPos > openParenPos) return std::nullopt;
        return previousStatementEnd(declaration, eqPos + 1);
    }
    std::string toLowerCopy(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {return static_cast<char>(std::tolower(c));});
        return value;
    }
    std::string removeWhitespaceCopy(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            if (std::isspace(static_cast<unsigned char>(c)) == 0) out.push_back(c);
        }
        return out;
    }
    std::string stripCommentsPreserveLayout(const std::string& source) {
        std::string out = source;
        enum class Mode {Normal, LineComment, BlockComment, StringLiteral, CharLiteral};
        Mode mode = Mode::Normal;
        bool escaped = false;
        for (size_t i = 0; i < out.size(); ++i) {
            char c = out[i];
            char next = (i + 1 < out.size()) ? out[i + 1] : '\0';
            switch (mode) {
                case Mode::Normal:
                    if (c == '/' && next == '/')      {mode = Mode::LineComment;  out[i] = ' '; out[i + 1] = ' '; ++i;}
                    else if (c == '/' && next == '*') {mode = Mode::BlockComment; out[i] = ' '; out[i + 1] = ' '; ++i;}
                    else if (c == '"')                {mode = Mode::StringLiteral;              escaped = false;}
                    else if (c == '\'')               {mode = Mode::CharLiteral;                escaped = false;}
                    break;
                case Mode::LineComment:
                    if (c == '\n') {mode = Mode::Normal;}
                    else {out[i] = ' ';}
                    break;
                case Mode::BlockComment:
                    if (c == '*' && next == '/') {out[i] = ' '; out[i + 1] = ' '; ++i; mode = Mode::Normal;}
                    else if (c != '\n') {out[i] = ' ';}
                    break;
                case Mode::StringLiteral:
                    if (escaped) {escaped = false;}
                    else if (c == '\\') {escaped = true;}
                    else if (c == '"') {mode = Mode::Normal;}
                    break;
                case Mode::CharLiteral:
                    if (escaped) {escaped = false;}
                    else if (c == '\\') {escaped = true;}
                    else if (c == '\'') {mode = Mode::Normal;}
                    break;
            }
        }

        return out;
    }

    size_t findMatchingBrace(const std::string& text, size_t openBracePos) {
        if (openBracePos >= text.size() || text[openBracePos] != '{') return std::string::npos;
        int depth = 1;
        bool inString = false;
        bool inChar = false;
        bool escaped = false;

        for (size_t i = openBracePos + 1; i < text.size(); ++i) {
            const char c = text[i];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
                continue;
            }
            if (inChar) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '\'') {
                    inChar = false;
                }
                continue;
            }

            if (c == '"') {
                inString = true;
                escaped = false;
                continue;
            }
            if (c == '\'') {
                inChar = true;
                escaped = false;
                continue;
            }
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    return i;
                }
            }
        }
        return std::string::npos;
    }

    size_t findMatchingParen(const std::string& text, size_t openParenPos) {
        if (openParenPos >= text.size() || text[openParenPos] != '(') return std::string::npos;
        int depth = 1;
        bool inString = false;
        bool inChar = false;
        bool escaped = false;

        for (size_t i = openParenPos + 1; i < text.size(); ++i) {
            const char c = text[i];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
                continue;
            }
            if (inChar) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '\'') {
                    inChar = false;
                }
                continue;
            }

            if (c == '"') {
                inString = true;
                escaped = false;
                continue;
            }
            if (c == '\'') {
                inChar = true;
                escaped = false;
                continue;
            }
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
                if (depth == 0) {
                    return i;
                }
            }
        }
        return std::string::npos;
    }

    size_t findMatchingBracket(const std::string& text, size_t openBracketPos) {
        if (openBracketPos >= text.size() || text[openBracketPos] != '[') return std::string::npos;
        int depth = 1;
        bool inString = false;
        bool inChar = false;
        bool escaped = false;

        for (size_t i = openBracketPos + 1; i < text.size(); ++i) {
            const char c = text[i];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
                continue;
            }
            if (inChar) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '\'') {
                    inChar = false;
                }
                continue;
            }

            if (c == '"') {
                inString = true;
                escaped = false;
                continue;
            }
            if (c == '\'') {
                inChar = true;
                escaped = false;
                continue;
            }
            if (c == '[') {
                ++depth;
            } else if (c == ']') {
                --depth;
                if (depth == 0) {
                    return i;
                }
            }
        }
        return std::string::npos;
    }

    size_t findTopLevelChar(const std::string& text, char needle) {
        int parenDepth = 0;
        int angleDepth = 0;
        int bracketDepth = 0;
        int braceDepth = 0;
        bool inString = false;
        bool inChar = false;
        bool escaped = false;
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
                continue;
            }
            if (inChar) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '\'') {
                    inChar = false;
                }
                continue;
            }

            if (c == '"') {
                inString = true;
                escaped = false;
                continue;
            }
            if (c == '\'') {
                inChar = true;
                escaped = false;
                continue;
            }

            if (c == needle && parenDepth == 0 && angleDepth == 0 &&
                bracketDepth == 0 && braceDepth == 0) {
                return i;
            }

            if (c == '(') ++parenDepth;
            else if (c == ')') parenDepth = std::max(0, parenDepth - 1);
            else if (c == '<') ++angleDepth;
            else if (c == '>') angleDepth = std::max(0, angleDepth - 1);
            else if (c == '[') ++bracketDepth;
            else if (c == ']') bracketDepth = std::max(0, bracketDepth - 1);
            else if (c == '{') ++braceDepth;
            else if (c == '}') braceDepth = std::max(0, braceDepth - 1);
        }
        return std::string::npos;
    }

    std::vector<std::string> splitTopLevel(const std::string& text, char delimiter) {
        std::vector<std::string> parts;
        std::string current;
        int parenDepth = 0;
        int angleDepth = 0;
        int bracketDepth = 0;
        int braceDepth = 0;
        bool inString = false;
        bool inChar = false;
        bool escaped = false;

        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (inString) {
                current.push_back(c);
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
                continue;
            }
            if (inChar) {
                current.push_back(c);
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '\'') {
                    inChar = false;
                }
                continue;
            }

            if (c == '"') {
                inString = true;
                escaped = false;
                current.push_back(c);
                continue;
            }
            if (c == '\'') {
                inChar = true;
                escaped = false;
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

            if (c == delimiter && parenDepth == 0 && angleDepth == 0 &&
                bracketDepth == 0 && braceDepth == 0) {
                parts.push_back(trimCopy(current));
                current.clear();
                continue;
            }

            current.push_back(c);
        }

        parts.push_back(trimCopy(current));
        return parts;
    }

    std::string escapeCStringLiteral(const std::string& value) {
        std::string out;
        out.reserve(value.size() + 8);
        for (char c : value) {
            if (c == '\\') out += "\\\\";
            else if (c == '"') out += "\\\"";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out.push_back(c);
        }
        return out;
    }

    // Converts $"..." raw multi-line string literals to escaped C++ string literals.
    // Inside a $"..." literal, actual newlines become \n, bare backslashes become \\,
    // and bare double-quotes are treated as the closing delimiter.
    // Already-escaped sequences (e.g. \n, \") are passed through unchanged.
    static std::string preprocessDollarStrings(const std::string& source) {
        std::string out;
        out.reserve(source.size());
        size_t i = 0;
        while (i < source.size()) {
            if (source[i] == '$' && i + 1 < source.size() && source[i + 1] == '"') {
                out.push_back('"');
                i += 2;
                while (i < source.size()) {
                    const char c = source[i];
                    if (c == '\\' && i + 1 < source.size()) {
                        out.push_back(c);
                        out.push_back(source[i + 1]);
                        i += 2;
                    } else if (c == '"') {
                        out.push_back('"');
                        ++i;
                        break;
                    } else if (c == '\r') {
                        ++i;
                    } else if (c == '\n') {
                        out += "\\n";
                        ++i;
                    } else {
                        out.push_back(c);
                        ++i;
                    }
                }
            } else {
                out.push_back(source[i]);
                ++i;
            }
        }
        return out;
    }

    static std::string unescapeAngleBracketTokens(const std::string& source) {
        if (source.find("[/L]") == std::string::npos &&
            source.find("[/R]") == std::string::npos) {
            return source;
        }
        std::string out;
        out.reserve(source.size());
        enum class Mode {Normal, LineComment, BlockComment, StringLiteral, CharLiteral};
        Mode mode = Mode::Normal;
        bool escaped = false;
        for (size_t i = 0; i < source.size(); ++i) {
            const char c = source[i];
            const char next = (i + 1 < source.size()) ? source[i + 1] : '\0';
            switch (mode) {
                case Mode::Normal:
                    if (c == '/' && next == '/')      {mode = Mode::LineComment;}
                    else if (c == '/' && next == '*') {mode = Mode::BlockComment;}
                    else if (c == '"')                {mode = Mode::StringLiteral; escaped = false;}
                    else if (c == '\'')               {mode = Mode::CharLiteral;   escaped = false;}
                    else if (c == '[' && source.compare(i, 4, "[/L]") == 0) {out.push_back('<'); i += 3; continue;}
                    else if (c == '[' && source.compare(i, 4, "[/R]") == 0) {out.push_back('>'); i += 3; continue;}
                    out.push_back(c);
                    break;
                case Mode::LineComment:
                    if (c == '\n') {mode = Mode::Normal;}
                    out.push_back(c);
                    break;
                case Mode::BlockComment:
                    if (c == '*' && next == '/') {out.push_back(c); out.push_back(next); ++i; mode = Mode::Normal; continue;}
                    out.push_back(c);
                    break;
                case Mode::StringLiteral:
                    if (escaped)        {escaped = false;}
                    else if (c == '\\') {escaped = true;}
                    else if (c == '"')  {mode = Mode::Normal;}
                    out.push_back(c);
                    break;
                case Mode::CharLiteral:
                    if (escaped)        {escaped = false;}
                    else if (c == '\\') {escaped = true;}
                    else if (c == '\'') {mode = Mode::Normal;}
                    out.push_back(c);
                    break;
            }
        }
        return out;
    }

    std::string inspectorLabelFromFieldName(const std::string& name) {
        std::string out;
        out.reserve(name.size() + 8);
        char prev = '\0';
        for (size_t i = 0; i < name.size(); ++i) {
            const char c = name[i];
            const bool isUpper = std::isupper(static_cast<unsigned char>(c)) != 0;
            const bool isDigit = std::isdigit(static_cast<unsigned char>(c)) != 0;
            const bool prevLower = std::islower(static_cast<unsigned char>(prev)) != 0;
            const bool prevDigit = std::isdigit(static_cast<unsigned char>(prev)) != 0;
            if (i > 0) {
                if (c == '_' || c == '-') {
                    if (!out.empty() && out.back() != ' ') out.push_back(' ');
                    prev = c;
                    continue;
                }
                if ((isUpper && prevLower) ||
                    (isDigit && !prevDigit) ||
                    (!isDigit && prevDigit)) {
                    if (!out.empty() && out.back() != ' ') out.push_back(' ');
                }
            }
            out.push_back(c);
            prev = c;
        }
        if (!out.empty()) {
            out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        }
        return out;
    }

    std::string identifierTailFromExpression(const std::string& expression) {
        std::string expr = trimCopy(expression);
        if (expr.empty()) return {};

        size_t end = expr.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(expr[end - 1])) != 0) {
            --end;
        }
        while (end > 0 &&
            (expr[end - 1] == ')' || expr[end - 1] == ']' || expr[end - 1] == ';')) {
            --end;
        }
        size_t start = end;
        while (start > 0 && isIdentifierChar(expr[start - 1])) {
            --start;
        }
        if (start >= end) return {};
        return expr.substr(start, end - start);
    }

    std::string inspectorLabelFromExpression(const std::string& expression) {
        std::string tail = identifierTailFromExpression(expression);
        if (tail.empty()) {
            return "Value";
        }
        return inspectorLabelFromFieldName(tail);
    }

    std::string extractParamName(const std::string& parameter) {
        if (parameter.empty()) return {};
        std::string preAssign = parameter;
        const size_t eqPos = findTopLevelChar(preAssign, '=');
        if (eqPos != std::string::npos) {
            preAssign = preAssign.substr(0, eqPos);
        }
        preAssign = trimCopy(preAssign);
        if (preAssign.empty()) return {};

        size_t end = preAssign.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(preAssign[end - 1])) != 0) {
            --end;
        }
        size_t start = end;
        while (start > 0 && isIdentifierChar(preAssign[start - 1])) {
            --start;
        }
        if (start >= end) return {};
        return preAssign.substr(start, end - start);
    }

    bool isListType(const std::string& rawType) {
        const std::string normalized = toLowerCopy(removeWhitespaceCopy(rawType));
        return normalized == "list<sceneobj*>" || normalized == "list<sceneobject*>";
    }

    bool isSceneObjectRefType(const std::string& rawType) {
        const std::string normalized = toLowerCopy(removeWhitespaceCopy(rawType));
        return normalized == "sceneobj*" || normalized == "sceneobject*" ||
            normalized == "sceneobj" || normalized == "sceneobject";
    }

    bool isLifecycleMethodName(const std::string& name) {
        static const std::unordered_set<std::string> kNames = {
            "Begin",
            "TickUpdate",
            "Update",
            "Spec",
            "TestEditor",
            "RenderEditorWindow",
            "ExitRenderEditorWindow",
            "Script_OnInspector",
            "OnCollideEnter",
            "OnCollideHold",
            "OnCollideExit"
        };
        return kNames.find(name) != kNames.end();
    }

    bool isCollisionMethodName(const std::string& name) {
        return name == "OnCollideEnter" ||
            name == "OnCollideHold" ||
            name == "OnCollideExit";
    }

    bool looksLikeDeltaTimeParameter(const std::string& parameter) {
        const std::string lower = toLowerCopy(removeWhitespaceCopy(parameter));
        return lower.find("floatdt") != std::string::npos ||
            lower.find("doubledt") != std::string::npos ||
            lower.find("floatdeltatime") != std::string::npos ||
            lower.find("doubledeltatime") != std::string::npos;
    }

    std::string mapScriptBaseTypeToCpp(const std::string& rawType) {
        std::string normalizedType = trimCopy(rawType);
        size_t dotPos = 0;
        while ((dotPos = normalizedType.find('.', dotPos)) != std::string::npos) {
            normalizedType.replace(dotPos, 1, "::");
            dotPos += 2;
        }

        const std::string normalized = toLowerCopy(removeWhitespaceCopy(normalizedType));
        if (normalized == "string") return "std::string";
        if (normalized == "vector2" || normalized == "vec2") return "glm::vec2";
        if (normalized == "vector3" || normalized == "vec3") return "glm::vec3";
        if (normalized == "col") return "SceneObject*";
        if (normalized == "sceneobj*" || normalized == "sceneobject*") return "SceneObject*";
        if (normalized == "sceneobj" || normalized == "sceneobject") return "SceneObject";
        return normalizedType;
    }

    bool parseArrayType(const std::string& rawType,
                        std::string& outBaseType,
                        std::vector<std::string>& outDimensions) {
        std::string remaining = trimCopy(rawType);
        outDimensions.clear();

        while (!remaining.empty() && remaining.back() == ']') {
            const size_t open = remaining.rfind('[');
            if (open == std::string::npos) {
                return false;
            }
            const std::string dim = trimCopy(remaining.substr(open + 1, remaining.size() - open - 2));
            outDimensions.insert(outDimensions.begin(), dim);
            remaining = trimCopy(remaining.substr(0, open));
        }

        outBaseType = remaining;
        return true;
    }

    bool parseListType(const std::string& rawType, std::string& outElementType) {
        std::string trimmed = trimCopy(rawType);
        if (trimmed.rfind("List<", 0) != 0 || trimmed.back() != '>') {
            return false;
        }

        const std::string inner = trimCopy(trimmed.substr(5, trimmed.size() - 6));
        if (inner.empty()) {
            return false;
        }

        outElementType = inner;
        return true;
    }

    FieldKind publicFieldKindForType(const std::string& rawType) {
        std::string baseType;
        std::vector<std::string> dims;
        if (!parseArrayType(rawType, baseType, dims)) {
            return FieldKind::Custom;
        }

        std::string listElementType;
        if (parseListType(baseType, listElementType)) {
            dims.push_back("");
            baseType = listElementType;
        }

        const std::string normalized = toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(baseType)));
        if (dims.empty() && (normalized == "sprite" || normalized == "::sprite")) {
            return FieldKind::SpriteRef;
        }
        if (dims.size() == 1 && trimCopy(dims[0]).empty()) {
            if (normalized == "dialogueport::dialogueline") return FieldKind::DialogueLines;
            if (normalized == "sceneobj*" || normalized == "sceneobject*" ||
                normalized == "sceneobj" || normalized == "sceneobject") {
                return FieldKind::ObjectList;
            }
            return FieldKind::Custom;
        }
        if (!dims.empty()) return FieldKind::Custom;

        if (normalized == "float") return FieldKind::Float;
        if (normalized == "int") return FieldKind::Int;
        if (normalized == "bool") return FieldKind::Bool;
        if (normalized == "vec3" || normalized == "glm::vec3" ||
            normalized == "vector3") return FieldKind::Vec3;
        if (normalized == "string" || normalized == "std::string") return FieldKind::String;
        if (isSceneObjectRefType(baseType)) return FieldKind::ObjectRef;
        if (isListType(baseType)) return FieldKind::ObjectList;
        return FieldKind::Custom;
    }

    std::string mapScriptTypeToCpp(const std::string& rawType) {
        std::string baseType;
        std::vector<std::string> dims;
        if (!parseArrayType(rawType, baseType, dims)) {
            return mapScriptBaseTypeToCpp(rawType);
        }

        std::string listElementType;
        if (parseListType(baseType, listElementType)) {
            dims.push_back("");
            baseType = listElementType;
        }

        std::string cppType = mapScriptBaseTypeToCpp(baseType);
        for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
            if (trimCopy(*it).empty()) {
                cppType = "std::vector<" + cppType + ">";
            } else {
                cppType = "std::array<" + cppType + ", " + *it + ">";
            }
        }
        return cppType;
    }

    bool parseFieldAttributeBlock(const std::string& attributeText, FieldSpec& outField, std::string& error) {
        std::string trimmed = trimCopy(attributeText);
        if (trimmed.empty()) {
            return true;
        }

        const size_t openParen = findTopLevelChar(trimmed, '(');
        std::string name = trimCopy(trimmed);
        std::string args;
        if (openParen != std::string::npos) {
            const size_t closeParen = findMatchingParen(trimmed, openParen);
            if (closeParen == std::string::npos) {
                error = "Field attribute has unmatched '(' near: [" + trimmed + "]";
                return false;
            }
            name = trimCopy(trimmed.substr(0, openParen));
            args = trimCopy(trimmed.substr(openParen + 1, closeParen - openParen - 1));
            if (!trimCopy(trimmed.substr(closeParen + 1)).empty()) {
                error = "Unexpected tokens after field attribute: [" + trimmed + "]";
                return false;
            }
        }

        if (name == "Header") {
            const std::vector<std::string> parts = args.empty() ? std::vector<std::string>{} : splitTopLevel(args, ',');
            if (parts.size() != 1) {
                error = "[Header(title)] requires exactly one argument.";
                return false;
            }
            outField.inspectorHeader = trimCopy(parts[0]);
            if (outField.inspectorHeader->empty()) {
                error = "[Header(title)] argument cannot be empty.";
                return false;
            }
            outField.hasInspectorMetadata = true;
            return true;
        }
        if (name == "Slider") {
            const std::vector<std::string> parts = splitTopLevel(args, ',');
            if (parts.size() != 2) {
                error = "[Slider(min, max)] requires exactly two arguments.";
                return false;
            }
            outField.hasSliderAttribute = true;
            outField.sliderMinExpr = trimCopy(parts[0]);
            outField.sliderMaxExpr = trimCopy(parts[1]);
            if (outField.sliderMinExpr->empty() || outField.sliderMaxExpr->empty()) {
                error = "[Slider(min, max)] arguments cannot be empty.";
                return false;
            }
            outField.hasInspectorMetadata = true;
            return true;
        }
        if (name == "ObjectRef") {
            if (!args.empty()) {
                error = "[ObjectRef] does not take arguments.";
                return false;
            }
            outField.hasObjectRefAttribute = true;
            outField.hasInspectorMetadata = true;
            return true;
        }
        if (name == "ObjectList") {
            if (!args.empty()) {
                error = "[ObjectList] does not take arguments.";
                return false;
            }
            outField.hasObjectListAttribute = true;
            outField.hasInspectorMetadata = true;
            return true;
        }
        if (name == "DialogueLines") {
            if (!args.empty()) {
                error = "[DialogueLines] does not take arguments.";
                return false;
            }
            outField.hasDialogueLinesAttribute = true;
            outField.hasInspectorMetadata = true;
            return true;
        }
        if (name == "ClipGridPair") {
            if (!args.empty()) {
                error = "[ClipGridPair] does not take arguments.";
                return false;
            }
            outField.hasClipGridPairAttribute = true;
            outField.hasInspectorMetadata = true;
            return true;
        }
        if (name == "Separator") {
            if (!args.empty()) {
                error = "[Separator] does not take arguments.";
                return false;
            }
            outField.hasSeparatorAttribute = true;
            outField.hasInspectorMetadata = true;
            return true;
        }
        if (name == "SoundSet") {
            const std::vector<std::string> parts = splitTopLevel(args, ',');
            if (parts.size() != 1) {
                error = "[SoundSet(label)] requires exactly one argument.";
                return false;
            }
            outField.soundSetLabel = trimCopy(parts[0]);
            if (outField.soundSetLabel->empty()) {
                error = "[SoundSet(label)] argument cannot be empty.";
                return false;
            }
            outField.hasInspectorMetadata = true;
            return true;
        }
        if (name == "range") {
            const std::vector<std::string> parts = splitTopLevel(args, ',');
            if (parts.size() != 2) {
                error = "@range requires two arguments: @range(min, max)";
                return false;
            }
            outField.rangeMinExpr = trimCopy(parts[0]);
            outField.rangeMaxExpr = trimCopy(parts[1]);
            if (outField.rangeMinExpr->empty() || outField.rangeMaxExpr->empty()) {
                error = "@range arguments cannot be empty.";
                return false;
            }
            return true;
        }
        if (name == "step") {
            const std::vector<std::string> parts = splitTopLevel(args, ',');
            if (parts.size() != 1) {
                error = "@step requires one argument: @step(value)";
                return false;
            }
            outField.stepExpr = trimCopy(parts[0]);
            if (outField.stepExpr->empty()) {
                error = "@step argument cannot be empty.";
                return false;
            }
            return true;
        }

        error = "Unsupported field attribute '" + name +
                "'. Supported attributes: [Header], [Slider], [ObjectRef], [ObjectList], [DialogueLines], [ClipGridPair], [Separator], [SoundSet], @range, @step.";
        return false;
    }

    bool stripAndParseFieldAnnotations(std::string& declaration, FieldSpec& outField, std::string& error) {
        std::string working = trimCopy(declaration);
        size_t cursor = 0;
        while (cursor < working.size()) {
            while (cursor < working.size() &&
                std::isspace(static_cast<unsigned char>(working[cursor])) != 0) {
                ++cursor;
            }
            if (cursor >= working.size() || working[cursor] != '[') {
                break;
            }
            const size_t close = findMatchingBracket(working, cursor);
            if (close == std::string::npos) {
                error = "Field attribute has unmatched '[' near: " + working.substr(cursor);
                return false;
            }
            if (!parseFieldAttributeBlock(working.substr(cursor + 1, close - cursor - 1), outField, error)) {
                return false;
            }
            cursor = close + 1;
        }

        std::string cleaned;
        cleaned.reserve(working.size());

        bool inString = false;
        bool inChar = false;
        bool escaped = false;

        size_t i = cursor;
        while (i < working.size()) {
            const char c = working[i];
            if (inString) {
                cleaned.push_back(c);
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
                ++i;
                continue;
            }
            if (inChar) {
                cleaned.push_back(c);
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '\'') {
                    inChar = false;
                }
                ++i;
                continue;
            }

            if (c == '"') {
                inString = true;
                escaped = false;
                cleaned.push_back(c);
                ++i;
                continue;
            }
            if (c == '\'') {
                inChar = true;
                escaped = false;
                cleaned.push_back(c);
                ++i;
                continue;
            }

            if (c != '@') {
                cleaned.push_back(c);
                ++i;
                continue;
            }

            size_t nameStart = i + 1;
            size_t nameEnd = nameStart;
            while (nameEnd < working.size()) {
                const unsigned char ch = static_cast<unsigned char>(working[nameEnd]);
                if (std::isalnum(ch) == 0 && working[nameEnd] != '_') break;
                ++nameEnd;
            }
            if (nameEnd == nameStart || nameEnd >= working.size() || working[nameEnd] != '(') {
                error = "Invalid field annotation syntax near: " + working.substr(i);
                return false;
            }

            const std::string annotationName = working.substr(nameStart, nameEnd - nameStart);
            const size_t argsOpen = nameEnd;
            const size_t argsClose = findMatchingParen(working, argsOpen);
            if (argsClose == std::string::npos) {
                error = "Field annotation has unmatched '(' near: " + working.substr(i);
                return false;
            }

            const std::string args = working.substr(argsOpen + 1, argsClose - argsOpen - 1);
            if (annotationName == "range") {
                const std::vector<std::string> parts = splitTopLevel(args, ',');
                if (parts.size() != 2) {
                    error = "@range requires two arguments: @range(min, max)";
                    return false;
                }
                outField.rangeMinExpr = trimCopy(parts[0]);
                outField.rangeMaxExpr = trimCopy(parts[1]);
                if (outField.rangeMinExpr->empty() || outField.rangeMaxExpr->empty()) {
                    error = "@range arguments cannot be empty.";
                    return false;
                }
            } else if (annotationName == "step") {
                const std::vector<std::string> parts = splitTopLevel(args, ',');
                if (parts.size() != 1) {
                    error = "@step requires one argument: @step(value)";
                    return false;
                }
                outField.stepExpr = trimCopy(parts[0]);
                if (outField.stepExpr->empty()) {
                    error = "@step argument cannot be empty.";
                    return false;
                }
            } else {
                error = "Unsupported field annotation '@" + annotationName +
                        "'. Supported annotations: @range(min, max), @step(value).";
                return false;
            }

            i = argsClose + 1;
        }

        declaration = trimCopy(cleaned);
        return true;
    }

    bool parseFieldDecl(const std::string& fieldDecl, FieldSpec& outField, std::string& error) {
        std::string trimmed = trimCopy(fieldDecl);
        if (trimmed.empty()) return false;
        if (trimmed.back() == ';') {
            trimmed.pop_back();
            trimmed = trimCopy(trimmed);
        }
        if (trimmed.empty()) return false;

        if (trimmed == "public:" || trimmed == "private:") {
            return false;
        }

        if (!stripAndParseFieldAnnotations(trimmed, outField, error)) {
            return false;
        }

        const std::string publicPrefix = "public ";
        const std::string privatePrefix = "private ";
        if (trimmed.rfind(publicPrefix, 0) == 0) {
            outField.visibility = FieldVisibility::Public;
            trimmed = trimCopy(trimmed.substr(publicPrefix.size()));
        } else if (trimmed.rfind(privatePrefix, 0) == 0) {
            outField.visibility = FieldVisibility::Private;
            trimmed = trimCopy(trimmed.substr(privatePrefix.size()));
        } else {
            error = "ModuCPP field is missing 'public' or 'private': " + fieldDecl;
            return false;
        }

        size_t eqPos = findTopLevelChar(trimmed, '=');
        std::string declPart = (eqPos == std::string::npos) ? trimmed : trimCopy(trimmed.substr(0, eqPos));
        outField.initializer = (eqPos == std::string::npos) ? std::string() : trimCopy(trimmed.substr(eqPos + 1));

        if (declPart.empty()) {
            error = "Invalid field declaration: " + fieldDecl;
            return false;
        }

        size_t nameEnd = declPart.size();
        while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(declPart[nameEnd - 1])) != 0) {
            --nameEnd;
        }
        size_t nameStart = nameEnd;
        while (nameStart > 0 && isIdentifierChar(declPart[nameStart - 1])) {
            --nameStart;
        }

        if (nameStart >= nameEnd) {
            error = "Unable to parse field name: " + fieldDecl;
            return false;
        }

        outField.name = declPart.substr(nameStart, nameEnd - nameStart);
        outField.rawType = trimCopy(declPart.substr(0, nameStart));
        if (outField.rawType.empty()) {
            error = "Unable to parse field type for: " + outField.name;
            return false;
        }
        if (!parseArrayType(outField.rawType, outField.baseType, outField.arrayDimensions)) {
            error = "Unable to parse array type for: " + outField.name;
            return false;
        }

        std::string listElementType;
        if (parseListType(outField.baseType, listElementType)) {
            outField.baseType = listElementType;
            outField.arrayDimensions.push_back("");
        }

        outField.kind = publicFieldKindForType(outField.rawType);
        if (outField.hasDialogueLinesAttribute) {
            outField.kind = FieldKind::DialogueLines;
        } else if (outField.hasObjectListAttribute) {
            outField.kind = FieldKind::ObjectList;
        } else if (outField.hasObjectRefAttribute && outField.kind == FieldKind::Custom) {
            outField.kind = FieldKind::String;
        }

        return true;
    }

    bool lowerParameterDecl(const std::string& rawParam, std::string& outLowered,
                            std::string& outParamName, bool& outIsContext, bool& outContextIsPointer,
                            bool& outIsDelta, std::string& error) {
        std::string param = trimCopy(rawParam);
        outLowered.clear();
        outParamName.clear();
        outIsContext = false;
        outContextIsPointer = false;
        outIsDelta = false;
        if (param.empty()) {
            return true;
        }

        if (param == "MODU_obj") {
            outLowered = "ScriptContext& ctx";
            outParamName = "ctx";
            outIsContext = true;
            return true;
        }

        const std::string lowerParam = toLowerCopy(param);
        if (lowerParam.find("scriptcontext") != std::string::npos) {
            outParamName = extractParamName(param);
            if (outParamName.empty()) {
                error = "Unable to parse ScriptContext parameter name.";
                return false;
            }
            outLowered = param;
            outIsContext = true;
            outContextIsPointer = param.find('*') != std::string::npos;
            return true;
        }

        outIsDelta = looksLikeDeltaTimeParameter(param);

        std::string defaultExpr;
        const size_t eqPos = findTopLevelChar(param, '=');
        if (eqPos != std::string::npos) {
            defaultExpr = trimCopy(param.substr(eqPos + 1));
            param = trimCopy(param.substr(0, eqPos));
        }

        bool isRef = false;
        if (param.rfind("ref ", 0) == 0) {
            isRef = true;
            param = trimCopy(param.substr(4));
        }

        outParamName = extractParamName(param);
        if (outParamName.empty()) {
            outLowered = rawParam;
            return true;
        }

        const size_t namePos = param.rfind(outParamName);
        if (namePos == std::string::npos) {
            outLowered = rawParam;
            return true;
        }

        std::string typePart = trimCopy(param.substr(0, namePos));
        if (typePart.empty()) {
            outLowered = rawParam;
            return true;
        }

        if (isRef) {
            typePart = mapScriptTypeToCpp(typePart) + "&";
        } else {
            typePart = mapScriptTypeToCpp(typePart);
        }

        outLowered = typePart + " " + outParamName;
        if (!defaultExpr.empty()) {
            outLowered += " = " + defaultExpr;
        }
        return true;
    }

    bool parameterHasExplicitType(const std::string& rawParam) {
        std::string param = trimCopy(rawParam);
        if (param.empty()) return true;
        const size_t eqPos = findTopLevelChar(param, '=');
        if (eqPos != std::string::npos) {
            param = trimCopy(param.substr(0, eqPos));
        }
        if (param.rfind("ref ", 0) == 0) {
            param = trimCopy(param.substr(4));
        }
        const std::string name = extractParamName(param);
        if (name.empty()) return true;
        const size_t namePos = param.rfind(name);
        if (namePos == std::string::npos) return true;
        return !trimCopy(param.substr(0, namePos)).empty();
    }

    std::string normalizeUntypedParameters(const std::string& params) {
        std::vector<std::string> parts = splitTopLevel(params, ',');
        std::ostringstream out;
        for (size_t i = 0; i < parts.size(); ++i) {
            std::string param = trimCopy(parts[i]);
            if (!param.empty() && !parameterHasExplicitType(param)) {
                param = "auto " + param;
            }
            if (i > 0) out << ", ";
            out << param;
        }
        return out.str();
    }

    bool normalizeCalcSignature(std::string& signature, bool& outIsCalc, std::string& error) {
        std::string trimmed = trimCopy(signature);
        outIsCalc = false;
        if (trimmed.rfind("Calc ", 0) != 0) {
            return true;
        }
        outIsCalc = true;

        trimmed = trimCopy(trimmed.substr(5));
        const size_t openParen = findTopLevelChar(trimmed, '(');
        if (openParen == std::string::npos) {
            error = "Unsupported Calc declaration (missing parameter list): " + signature;
            return false;
        }
        const size_t closeParen = findMatchingParen(trimmed, openParen);
        if (closeParen == std::string::npos) {
            error = "Unsupported Calc declaration (unmatched '('): " + signature;
            return false;
        }

        const std::string beforeParen = trimCopy(trimmed.substr(0, openParen));
        size_t nameEnd = beforeParen.size();
        while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(beforeParen[nameEnd - 1])) != 0) {
            --nameEnd;
        }
        size_t nameStart = nameEnd;
        while (nameStart > 0 && isIdentifierChar(beforeParen[nameStart - 1])) {
            --nameStart;
        }
        if (nameStart >= nameEnd) {
            error = "Unsupported Calc declaration (missing name): " + signature;
            return false;
        }

        const std::string returnType = trimCopy(beforeParen.substr(0, nameStart)).empty()
            ? "auto"
            : trimCopy(beforeParen.substr(0, nameStart));
        const std::string name = beforeParen.substr(nameStart, nameEnd - nameStart);
        const std::string params = normalizeUntypedParameters(trimmed.substr(openParen + 1, closeParen - openParen - 1));
        const std::string trailing = trimCopy(trimmed.substr(closeParen + 1));

        signature = returnType + " " + name + "(" + params + ")";
        if (!trailing.empty()) {
            signature += " " + trailing;
        }
        return true;
    }

    bool normalizeCollisionSignature(std::string& signature, std::string& outHoldDuration) {
        outHoldDuration.clear();
        std::string trimmed = trimCopy(signature);
        const size_t openParen = findTopLevelChar(trimmed, '(');
        if (openParen == std::string::npos) {
            return false;
        }
        const std::string beforeParen = trimCopy(trimmed.substr(0, openParen));
        if (!isCollisionMethodName(beforeParen)) {
            return false;
        }
        const size_t closeParen = findMatchingParen(trimmed, openParen);
        if (closeParen == std::string::npos) {
            return false;
        }

        std::string params = trimCopy(trimmed.substr(openParen + 1, closeParen - openParen - 1));
        std::vector<std::string> parts = splitTopLevel(params, ',');
        if (parts.size() == 1) {
            static const std::regex holdDurationPattern(
                R"(^\s*Col\s+([A-Za-z_][A-Za-z0-9_]*)\s+([0-9]+(?:\.[0-9]+)?f?)\s*$)");
            std::smatch match;
            if (std::regex_match(parts[0], match, holdDurationPattern)) {
                parts[0] = "Col " + match[1].str();
                outHoldDuration = match[2].str();
            }
            params = trimCopy(parts[0]);
        }
        if (beforeParen == "OnCollideHold" && outHoldDuration.empty()) {
            outHoldDuration = "2.0f";
        }

        signature = "void " + beforeParen + "(" + params + ")" + trimmed.substr(closeParen + 1);
        return true;
    }

    bool parseMethodDecl(const std::string& signatureDecl, const std::string& body,
                        int bodySourceLine,
                        MethodSpec& outMethod, std::string& error) {
        std::string signature = trimCopy(signatureDecl);
        if (signature.empty()) return false;

        const std::string publicPrefix = "public ";
        const std::string privatePrefix = "private ";
        if (signature.rfind(publicPrefix, 0) == 0) {
            signature = trimCopy(signature.substr(publicPrefix.size()));
        } else if (signature.rfind(privatePrefix, 0) == 0) {
            signature = trimCopy(signature.substr(privatePrefix.size()));
        }
        bool isCalc = false;
        if (!normalizeCalcSignature(signature, isCalc, error)) {
            return false;
        }
        outMethod.isCalc = isCalc;
        normalizeCollisionSignature(signature, outMethod.collisionHoldDuration);

        const size_t openParen = findTopLevelChar(signature, '(');
        if (openParen == std::string::npos) {
            error = "Unsupported ModuCPP method declaration: " + signature;
            return false;
        }
        const size_t closeParen = findMatchingParen(signature, openParen);
        if (closeParen == std::string::npos) {
            error = "Unsupported ModuCPP method declaration (unmatched '('): " + signature;
            return false;
        }
        const std::string trailing = trimCopy(signature.substr(closeParen + 1));
        bool hasExpressionBody = false;
        std::string expressionBody;
        if (!trailing.empty()) {
            if (trailing.rfind("to ", 0) != 0) {
                error = "Unsupported ModuCPP method declaration (trailing tokens): " + signature;
                return false;
            }
            hasExpressionBody = true;
            expressionBody = trimCopy(trailing.substr(3));
            if (expressionBody.empty()) {
                error = "Expression-bodied member is missing its body: " + signature;
                return false;
            }
        }

        const std::string beforeParen = trimCopy(signature.substr(0, openParen));
        if (beforeParen.empty()) {
            error = "Unsupported ModuCPP method declaration (missing return type/name): " + signature;
            return false;
        }

        size_t nameEnd = beforeParen.size();
        while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(beforeParen[nameEnd - 1])) != 0) {
            --nameEnd;
        }
        size_t nameStart = nameEnd;
        while (nameStart > 0 && isIdentifierChar(beforeParen[nameStart - 1])) {
            --nameStart;
        }
        if (nameStart >= nameEnd) {
            error = "Unsupported ModuCPP method declaration (missing method name): " + signature;
            return false;
        }

        outMethod.name = beforeParen.substr(nameStart, nameEnd - nameStart);
        {
            std::string rawReturn = trimCopy(beforeParen.substr(0, nameStart));
            static const std::regex staticPrefix(R"(^static\s+)");
            if (std::regex_search(rawReturn, staticPrefix)) {
                outMethod.isStatic = true;
                rawReturn = std::regex_replace(rawReturn, staticPrefix, "");
            }
            outMethod.returnType = mapScriptTypeToCpp(rawReturn);
        }
        if (outMethod.returnType.empty()) {
            error = "Unsupported ModuCPP method declaration (missing return type): " + signature;
            return false;
        }
        outMethod.originalParams = trimCopy(signature.substr(openParen + 1, closeParen - openParen - 1));
        outMethod.body = body;
        outMethod.bodySourceLine = bodySourceLine;
        if (hasExpressionBody) {
            outMethod.body = isVoidReturnType(outMethod.returnType)
                ? (expressionBody + ";")
                : ("return " + expressionBody + ";");
        }
        {
            const std::string bodyStripped = stripCommentsPreserveLayout(outMethod.body);
            static const std::regex moduScriptPattern(R"(\bMODU_SCRIPT\s*\()");
            outMethod.hasManualPrelude = std::regex_search(bodyStripped, moduScriptPattern);
        }

        std::vector<std::string> params = splitTopLevel(outMethod.originalParams, ',');
        std::vector<std::string> transpiledParams;
        transpiledParams.reserve(params.size());

        for (const std::string& rawParam : params) {
            std::string loweredParam;
            std::string paramName;
            bool isContext = false;
            bool contextIsPointer = false;
            bool isDelta = false;
            if (!lowerParameterDecl(rawParam, loweredParam, paramName, isContext, contextIsPointer,
                                    isDelta, error)) {
                error = "Unable to parse parameter in method '" + outMethod.name + "': " + error;
                return false;
            }
            if (trimCopy(loweredParam).empty()) continue;

            if (isContext) {
                if (outMethod.hasContext) {
                    error = "Multiple context parameters found in method: " + outMethod.name;
                    return false;
                }
                outMethod.hasContext = true;
                outMethod.contextParamName = paramName.empty() ? "ctx" : paramName;
                outMethod.contextIsPointer = contextIsPointer;
            }
            if (isDelta && !paramName.empty()) {
                outMethod.deltaTimeParamName = paramName;
                outMethod.hasDeltaTimeParam = true;
            }

            transpiledParams.push_back(loweredParam);
        }

        if (isLifecycleMethodName(outMethod.name)) {
            if (!outMethod.hasContext) {
                outMethod.hasContext = true;
                outMethod.contextParamName = "ctx";
                outMethod.contextIsPointer = false;
                outMethod.autoInjectedContext = true;
                transpiledParams.insert(transpiledParams.begin(), "ScriptContext& ctx");
            }

            if (outMethod.name != "Script_OnInspector") {
                bool hasDelta = false;
                for (const std::string& rawParam : params) {
                    if (looksLikeDeltaTimeParameter(rawParam)) {
                        hasDelta = true;
                        break;
                    }
                }
                if (!hasDelta && outMethod.name != "RenderEditorWindow" &&
                    outMethod.name != "ExitRenderEditorWindow") {
                    outMethod.autoInjectedDeltaTime = true;
                    outMethod.deltaTimeParamName = "dt";
                    outMethod.hasDeltaTimeParam = true;
                    transpiledParams.push_back("float dt");
                }
            }
        }

        std::ostringstream joined;
        for (size_t i = 0; i < transpiledParams.size(); ++i) {
            if (i > 0) joined << ", ";
            joined << transpiledParams[i];
        }
        outMethod.transpiledParams = joined.str();
        return true;
    }

    void replaceRegexAll(std::string& text, const std::regex& pattern, const std::string& replacement);

    // Narrow targeted rewrites for known [ObjectRef] string fields.
    // Only the field names in objectRefFields are touched. Locals, regular bools,
    // and SceneObj handles are intentionally left alone.
    //
    // Supported forms (FIELD is one of objectRefFields):
    //   FIELD.UI.Position = expr;        ->  ::ModuCPP::SetUIPosition(FIELD, expr);
    //   FIELD.UI.Size     = expr;        ->  ::ModuCPP::SetUISize(FIELD, expr);
    //   FIELD.UI.Position                ->  ::ModuCPP::UIPosition(FIELD)
    //   FIELD.UI.Size                    ->  ::ModuCPP::UISize(FIELD)
    //   FIELD.UI.Exists                  ->  ::ModuCPP::UIExists(FIELD)
    //   if (FIELD)                       ->  if (!FIELD.empty())
    //   if (!FIELD)                      ->  if (FIELD.empty())
    //   ... !FIELD ... (inline, between ( || && and space/||/&&/) )
    //                                    ->  ... FIELD.empty() ...
    inline std::string regexEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() * 2);
        for (char c : s) {
            if (c == '.' || c == '\\' || c == '+' || c == '*' || c == '?' ||
                c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
                c == '}' || c == '^' || c == '$' || c == '|' || c == '/') {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        return out;
    }

    std::string transformObjectRefAccess(const std::string& body,
                                        const std::unordered_set<std::string>& objectRefFields) {
        if (objectRefFields.empty()) return body;
        std::string out = body;
        for (const std::string& field : objectRefFields) {
            if (out.find(field) == std::string::npos) continue;

            const std::string esc = regexEscape(field);

            // 1. UI assignments (must run BEFORE the UI read rewrites so we don't
            //    accidentally rewrite the LHS `FIELD.UI.Position` as a read first).
            replaceRegexAll(out,
                std::regex("\\b" + esc + "\\s*\\.\\s*UI\\s*\\.\\s*Position\\s*=\\s*([^;]+);"),
                "::ModuCPP::SetUIPosition(" + field + ", $1);");
            replaceRegexAll(out,
                std::regex("\\b" + esc + "\\s*\\.\\s*UI\\s*\\.\\s*Size\\s*=\\s*([^;]+);"),
                "::ModuCPP::SetUISize(" + field + ", $1);");

            // 2. UI reads.
            replaceRegexAll(out,
                std::regex("\\b" + esc + "\\s*\\.\\s*UI\\s*\\.\\s*Position\\b"),
                "::ModuCPP::UIPosition(" + field + ")");
            replaceRegexAll(out,
                std::regex("\\b" + esc + "\\s*\\.\\s*UI\\s*\\.\\s*Size\\b"),
                "::ModuCPP::UISize(" + field + ")");
            replaceRegexAll(out,
                std::regex("\\b" + esc + "\\s*\\.\\s*UI\\s*\\.\\s*Exists\\b"),
                "::ModuCPP::UIExists(" + field + ")");

            // 3. Truthiness in plain `if (...)` heads.
            replaceRegexAll(out,
                std::regex("\\bif\\s*\\(\\s*!\\s*" + esc + "\\s*\\)"),
                "if (" + field + ".empty())");
            replaceRegexAll(out,
                std::regex("\\bif\\s*\\(\\s*" + esc + "\\s*\\)"),
                "if (!" + field + ".empty())");

            // 4. Inline `!FIELD` in compound conditions (e.g. `if (!a || !b.UI.Exists)`).
            //    Match `!FIELD` not preceded by an identifier char and followed by a
            //    boolean separator. ECMAScript regex supports lookahead but not
            //    lookbehind, so the prefix char is captured and re-emitted.
            replaceRegexAll(out,
                std::regex("([^A-Za-z0-9_.])!\\s*" + esc + "(?=[\\s\\|\\&\\)])"),
                "$1" + field + ".empty()");
        }
        return out;
    }

    std::string transformSpriteAssignment(const std::string& body,
                                          const std::unordered_set<std::string>& objectRefFields) {
        if (body.find("Sprite") == std::string::npos) return body;
        std::string out = body;

        replaceRegexAll(out,
            std::regex("\\bself\\s*\\.\\s*Sprite\\s*=\\s*([^;=][^;]*);"),
            "::ModuCPP::SetObjectSprite(::ModuCPP::Scene::Current(), $1);");

        for (const std::string& field : objectRefFields) {
            const std::string esc = regexEscape(field);
            replaceRegexAll(out,
                std::regex("\\b" + esc + "\\s*\\.\\s*Sprite\\s*=\\s*([^;=][^;]*);"),
                "::ModuCPP::SetObjectSprite(::ModuCPP::Scene::Resolve(::ModuCPP::Field::Text(\"" +
                    field + "\")), $1);");
        }

        replaceRegexAll(out,
            std::regex("\\b([A-Za-z_]\\w*)\\s*\\.\\s*Sprite\\s*=\\s*([^;=][^;]*);"),
            "::ModuCPP::SetObjectSprite($1, $2);");
        return out;
    }

    // Lowers the long form
    //   each <var> in <listExpr> then <var>.State(<expr>);
    // to the existing short form
    //   each <listExpr>.state(<expr>);
    std::string lowerEachInSyntax(const std::string& body) {
        if (body.find("each") == std::string::npos) return body;

        static const std::regex eachInPattern(
            R"(each\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+([A-Za-z_][A-Za-z0-9_]*(?:\s*\.\s*[A-Za-z_][A-Za-z0-9_]*)*)\s+then\s+\1\s*\.\s*[Ss]tate\s*\(\s*([^\)]+?)\s*\)\s*;)");
        std::string out = body;
        replaceRegexAll(out, eachInPattern, "each $2.state($3);");
        return out;
    }

    std::string transformEachSyntax(const std::string& body,
                                    const std::unordered_set<std::string>& listFields,
                                    const std::string& supportNamespace,
                                    bool hasContext,
                                    std::string& error) {
        if (body.find("each") == std::string::npos) return body;

        auto startsWithWord = [&](size_t pos, const std::string& word) {
            if (pos + word.size() > body.size()) return false;
            for (size_t i = 0; i < word.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(body[pos + i])) !=
                    std::tolower(static_cast<unsigned char>(word[i]))) {
                    return false;
                }
            }
            return pos + word.size() >= body.size() || !isIdentifierChar(body[pos + word.size()]);
        };

        std::string out;
        size_t cursor = 0;
        size_t searchPos = 0;
        while (searchPos < body.size()) {
            const size_t matchPos = body.find("each", searchPos);
            if (matchPos == std::string::npos) break;
            const size_t eachEnd = matchPos + 4;
            if ((matchPos > 0 && isIdentifierChar(body[matchPos - 1])) ||
                (eachEnd < body.size() && isIdentifierChar(body[eachEnd]))) {
                searchPos = eachEnd;
                continue;
            }

            size_t pos = skipWhitespace(body, eachEnd);
            if (pos >= body.size() || !isIdentifierStartChar(body[pos])) {
                searchPos = eachEnd;
                continue;
            }

            const size_t listStart = pos;
            size_t listEnd = std::string::npos;
            while (pos < body.size()) {
                if (!isIdentifierStartChar(body[pos])) break;
                ++pos;
                while (pos < body.size() && isIdentifierChar(body[pos])) {
                    ++pos;
                }

                const size_t dotPos = skipWhitespace(body, pos);
                if (dotPos >= body.size() || body[dotPos] != '.') break;

                const size_t afterDot = skipWhitespace(body, dotPos + 1);
                if (startsWithWord(afterDot, "state")) {
                    const size_t openParen = skipWhitespace(body, afterDot + 5);
                    if (openParen >= body.size() || body[openParen] != '(') break;
                    const size_t closeParen = findMatchingParen(body, openParen);
                    if (closeParen == std::string::npos) break;
                    const size_t semiPos = skipWhitespace(body, closeParen + 1);
                    if (semiPos >= body.size() || body[semiPos] != ';') break;
                    listEnd = dotPos;
                    pos = semiPos + 1;
                    break;
                }

                if (afterDot >= body.size() || !isIdentifierStartChar(body[afterDot])) break;
                pos = afterDot;
            }

            if (listEnd == std::string::npos) {
                searchPos = eachEnd;
                continue;
            }

            out += body.substr(cursor, matchPos - cursor);

            std::string listName = body.substr(listStart, listEnd - listStart);
            listName.erase(std::remove_if(listName.begin(), listName.end(),
                                        [](unsigned char c) { return std::isspace(c) != 0; }),
                        listName.end());
            const size_t openParen = body.find('(', listEnd);
            const size_t closeParen = findMatchingParen(body, openParen);
            const std::string enabledExpr = trimCopy(body.substr(openParen + 1, closeParen - openParen - 1));
            // For dotted accesses (e.g. action.disable from a SubScript), skip the
            // The C++ expression is still type-checked at compile time.
            const bool isDottedAccess = listName.find('.') != std::string::npos;
            if (!isDottedAccess && listFields.find(listName) == listFields.end()) {
                error = "Unknown list field in each-expression: " + listName;
                return {};
            }
            const size_t lineStartPos = body.rfind('\n', matchPos);
            const size_t indentStart = (lineStartPos == std::string::npos) ? 0 : lineStartPos + 1;
            size_t indentEnd = indentStart;
            while (indentEnd < matchPos &&
                (body[indentEnd] == ' ' || body[indentEnd] == '\t')) {
                ++indentEnd;
            }
            const std::string indent = body.substr(indentStart, indentEnd - indentStart);

            if (hasContext) {
                out += indent + "for (SceneObject* _moduObj : " + supportNamespace +
                    "::ResolveObjectList(ctx, " + listName + ")) {\n";
                out += indent + "    if (!_moduObj) continue;\n";
                out += indent + "    " + supportNamespace +
                    "::SetResolvedObjectEnabled(ctx, _moduObj, (" + enabledExpr + "));\n";
                out += indent + "}\n";
            } else {
                // No ctx parameter (user-defined helper method). Resolve through the
                // thread-local script context pointer instead of erroring.
                out += indent + "if (auto* _moduCtx = ::ModuCPP::ctxPtr()) {\n";
                out += indent + "    for (SceneObject* _moduObj : " + supportNamespace +
                    "::ResolveObjectList(*_moduCtx, " + listName + ")) {\n";
                out += indent + "        if (!_moduObj) continue;\n";
                out += indent + "        " + supportNamespace +
                    "::SetResolvedObjectEnabled(*_moduCtx, _moduObj, (" + enabledExpr + "));\n";
                out += indent + "    }\n";
                out += indent + "}\n";
            }

            cursor = pos;
            searchPos = pos;
        }

        out += body.substr(cursor);
        return out;
    }

    std::string sanitizeIdentifier(const std::string& input) {
        std::string out;
        out.reserve(input.size() + 8);
        for (char c : input) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
                out.push_back(c);
            } else {
                out.push_back('_');
            }
        }
        if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front())) != 0) {
            out.insert(out.begin(), '_');
        }
        return out;
    }

    bool isVoidReturnType(const std::string& returnType) {
        const std::string normalized = toLowerCopy(removeWhitespaceCopy(returnType));
        return normalized == "void";
    }

    std::string rewriteIncludeDirective(const std::string& directive, const fs::path& sourcePath) {
        static const std::regex includePattern(R"(^\s*#include\s*([<"])([^>"]+)[>"]\s*$)");
        std::smatch match;
        if (!std::regex_match(directive, match, includePattern)) {
            return trimCopy(directive);
        }

        const std::string bracket = match[1].str();
        const std::string includeTarget = trimCopy(match[2].str());
        if (includeTarget.empty()) {
            return trimCopy(directive);
        }
        static const std::unordered_map<std::string, std::string> packageIncludes = {
            {"ModuCPP", "ModuCPPScriptApi.h"},
            {"ModuEngine", "ModuEngineScriptApi.h"},
            {"ModuInput", "ModuInputScriptApi.h"},
            {"ModuInput.XR", "ModuInputXRScriptApi.h"},
            {"ModuNetwork", "ModuNetworkScriptApi.h"},
            {"RMeshBuilder", "RMeshBuilderScriptApi.h"},
            {"ModuCPP.Experimental", "ModuCPPExperimentalScriptApi.h"},
        };
        if (const auto it = packageIncludes.find(includeTarget); it != packageIncludes.end()) {
            return "#include \"" + it->second + "\"";
        }

        if (bracket == "\"") {
            std::error_code ec;
            fs::path dir = sourcePath.parent_path();
            while (!dir.empty()) {
                fs::path candidate = dir / includeTarget;
                fs::path absolute = fs::absolute(candidate, ec);
                if (!ec && fs::exists(absolute, ec) && !ec) {
                    return "#include \"" + absolute.lexically_normal().generic_string() + "\"";
                }
                fs::path parent = dir.parent_path();
                if (parent == dir) {
                    break;
                }
                dir = std::move(parent);
                ec.clear();
            }
        }

        return "#include " + bracket + includeTarget + ((bracket == "<") ? ">" : "\"");
    }

    fs::path findNearestSiblingHeader(const fs::path& sourcePath, const std::string& headerName) {
        std::error_code ec;
        fs::path dir = sourcePath.parent_path();
        while (!dir.empty()) {
            fs::path candidate = dir / headerName;
            if (fs::exists(candidate, ec) && !ec) {
                return candidate;
            }
            fs::path parent = dir.parent_path();
            if (parent == dir) {
                break;
            }
            dir = std::move(parent);
            ec.clear();
        }
        return sourcePath.parent_path() / headerName;
    }

    std::string stripIncludeDirectives(const std::string& sourceText) {
        std::istringstream input(sourceText);
        std::ostringstream out;
        std::string line;
        while (std::getline(input, line)) {
            const std::string trimmed = trimCopy(line);
            if (trimmed.rfind("#include", 0) == 0) {
                continue;
            }
            out << line << "\n";
        }
        return out.str();
    }

    std::string withDefaultFieldVisibility(const std::string& declaration, const char* visibilityKeyword) {
        std::string trimmed = trimCopy(declaration);
        size_t cursor = 0;
        while (cursor < trimmed.size()) {
            cursor = skipWhitespace(trimmed, cursor);
            if (cursor >= trimmed.size() || trimmed[cursor] != '[') {
                break;
            }
            const size_t close = findMatchingBracket(trimmed, cursor);
            if (close == std::string::npos) {
                break;
            }
            cursor = close + 1;
        }

        const size_t tokenStart = skipWhitespace(trimmed, cursor);
        if (trimmed.compare(tokenStart, 7, "public ") == 0 ||
            trimmed.compare(tokenStart, 8, "private ") == 0 ||
            trimmed.compare(tokenStart, 7, "public:") == 0 ||
            trimmed.compare(tokenStart, 8, "private:") == 0) {
            return trimmed;
        }

        return trimCopy(trimmed.substr(0, tokenStart)) +
            (tokenStart > 0 ? " " : "") +
            std::string(visibilityKeyword) + " " +
            trimmed.substr(tokenStart);
    }

    bool tryParseSubScriptStruct(const std::string& structName,
                                const std::string& bodyRaw,
                                const std::string& bodyClean,
                                SubScriptSpec& outSpec) {
        outSpec.name = structName;
        outSpec.rawDefinition = bodyRaw;

        std::set<std::string> fieldNames;
        size_t i = 0;
        while (i < bodyClean.size()) {
            while (i < bodyClean.size() && std::isspace(static_cast<unsigned char>(bodyClean[i])) != 0) {
                ++i;
            }
            if (i >= bodyClean.size()) break;

            size_t nextSemicolon = std::string::npos;
            size_t nextOpenBrace = std::string::npos;
            int parenDepth = 0;
            int angleDepth = 0;
            int bracketDepth = 0;
            int braceDepth = 0;
            bool inString = false;
            bool inChar = false;
            bool escaped = false;

            for (size_t j = i; j < bodyClean.size(); ++j) {
                const char c = bodyClean[j];
                if (inString) {
                    if (escaped) escaped = false;
                    else if (c == '\\') escaped = true;
                    else if (c == '"') inString = false;
                    continue;
                }
                if (inChar) {
                    if (escaped) escaped = false;
                    else if (c == '\\') escaped = true;
                    else if (c == '\'') inChar = false;
                    continue;
                }

                if (c == '"') {
                    inString = true;
                    escaped = false;
                    continue;
                }
                if (c == '\'') {
                    inChar = true;
                    escaped = false;
                    continue;
                }

                if (c == '(') ++parenDepth;
                else if (c == ')') angleDepth = 0, parenDepth = std::max(0, parenDepth - 1);
                else if (c == '<') ++angleDepth;
                else if (c == '>') angleDepth = std::max(0, angleDepth - 1);
                else if (c == '[') ++bracketDepth;
                else if (c == ']') bracketDepth = std::max(0, bracketDepth - 1);
                else if (c == '{') {
                    if (parenDepth == 0 && angleDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
                        nextOpenBrace = j;
                        break;
                    }
                    ++braceDepth;
                } else if (c == '}') {
                    braceDepth = std::max(0, braceDepth - 1);
                }

                if (parenDepth == 0 && angleDepth == 0 && bracketDepth == 0 && braceDepth == 0 && c == ';') {
                    nextSemicolon = j;
                    break;
                }
            }

            if (nextSemicolon == std::string::npos && nextOpenBrace == std::string::npos) {
                break;
            }

            if (nextOpenBrace != std::string::npos &&
                (nextSemicolon == std::string::npos || nextOpenBrace < nextSemicolon)) {
                const size_t methodCloseBrace = findMatchingBrace(bodyClean, nextOpenBrace);
                if (methodCloseBrace == std::string::npos) {
                    return false;
                }
                i = methodCloseBrace + 1;
                if (i < bodyClean.size() && bodyClean[i] == ';') {
                    ++i;
                }
                continue;
            }

            std::string statementDecl = bodyRaw.substr(i, nextSemicolon - i + 1);
            const std::string statementTrimmed = trimCopy(statementDecl);
            if (statementTrimmed.empty() || statementTrimmed == "public:" || statementTrimmed == "private:") {
                i = nextSemicolon + 1;
                continue;
            }

            FieldSpec field;
            std::string fieldError;
            if (!parseFieldDecl(withDefaultFieldVisibility(statementDecl, "public"), field, fieldError)) {
                i = nextSemicolon + 1;
                continue;
            }
            if (!fieldNames.insert(field.name).second) {
                return false;
            }
            outSpec.fields.push_back(std::move(field));
            i = nextSemicolon + 1;
        }

        return !outSpec.fields.empty();
    }

    std::vector<SubScriptSpec> parseSubScriptStructs(const std::string& sourceText) {
        std::vector<SubScriptSpec> subScripts;
        const std::string stripped = stripCommentsPreserveLayout(sourceText);
        static const std::regex structPattern(R"(\bstruct\s+([A-Za-z_][A-Za-z0-9_]*)\b)");

        for (auto it = std::sregex_iterator(stripped.begin(), stripped.end(), structPattern);
            it != std::sregex_iterator(); ++it) {
            const std::smatch& match = *it;
            const size_t matchPos = static_cast<size_t>(match.position());
            const size_t matchLen = static_cast<size_t>(match.length());
            const size_t openBrace = stripped.find('{', matchPos + matchLen);
            if (openBrace == std::string::npos) {
                continue;
            }
            const size_t closeBrace = findMatchingBrace(stripped, openBrace);
            if (closeBrace == std::string::npos) {
                continue;
            }

            SubScriptSpec spec;
            const std::string bodyRaw = sourceText.substr(openBrace + 1, closeBrace - openBrace - 1);
            const std::string bodyClean = stripped.substr(openBrace + 1, closeBrace - openBrace - 1);
            if (!tryParseSubScriptStruct(match[1].str(), bodyRaw, bodyClean, spec)) {
                continue;
            }
            subScripts.push_back(std::move(spec));
        }

        return subScripts;
    }

    std::string stripStructDefinitionsByName(const std::string& sourceText,
                                            const std::unordered_set<std::string>& structNames) {
        if (structNames.empty()) return sourceText;

        std::string stripped = stripCommentsPreserveLayout(sourceText);
        std::string result = sourceText;
        static const std::regex structPattern(R"(\bstruct\s+([A-Za-z_][A-Za-z0-9_]*)\b)");

        size_t searchPos = 0;
        while (searchPos < stripped.size()) {
            std::smatch match;
            auto begin = stripped.cbegin() + static_cast<std::ptrdiff_t>(searchPos);
            if (!std::regex_search(begin, stripped.cend(), match, structPattern)) {
                break;
            }

            const size_t matchPos = searchPos + static_cast<size_t>(match.position());
            const size_t matchLen = static_cast<size_t>(match.length());
            const std::string structName = match[1].str();
            if (structNames.find(structName) == structNames.end()) {
                searchPos = matchPos + matchLen;
                continue;
            }

            const size_t openBrace = stripped.find('{', matchPos + matchLen);
            if (openBrace == std::string::npos) {
                searchPos = matchPos + matchLen;
                continue;
            }
            const size_t closeBrace = findMatchingBrace(stripped, openBrace);
            if (closeBrace == std::string::npos) {
                searchPos = matchPos + matchLen;
                continue;
            }

            size_t endPos = closeBrace + 1;
            while (endPos < stripped.size() &&
                std::isspace(static_cast<unsigned char>(stripped[endPos])) != 0) {
                ++endPos;
            }
            if (endPos < stripped.size() && stripped[endPos] == ';') {
                ++endPos;
            }

            for (size_t i = matchPos; i < endPos; ++i) {
                result[i] = ' ';
                stripped[i] = ' ';
            }
            searchPos = endPos;
        }

        return result;
    }

    struct ExtractedCodeBlocks {
        std::string extracted;
        std::string remaining;
    };

    ExtractedCodeBlocks extractEnumClassDefinitions(const std::string& sourceText) {
        const std::string stripped = stripCommentsPreserveLayout(sourceText);
        static const std::regex enumPattern(R"(\benum\s+class\s+([A-Za-z_][A-Za-z0-9_]*)\b)");

        ExtractedCodeBlocks out;
        size_t cursor = 0;
        auto begin = std::sregex_iterator(stripped.begin(), stripped.end(), enumPattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::smatch& match = *it;
            const size_t matchPos = static_cast<size_t>(match.position());
            if (matchPos < cursor) {
                continue;
            }

            const size_t matchLen = static_cast<size_t>(match.length());
            const size_t openBrace = stripped.find('{', matchPos + matchLen);
            if (openBrace == std::string::npos) {
                continue;
            }
            const size_t closeBrace = findMatchingBrace(stripped, openBrace);
            if (closeBrace == std::string::npos) {
                continue;
            }

            size_t endPos = closeBrace + 1;
            while (endPos < stripped.size() &&
                std::isspace(static_cast<unsigned char>(stripped[endPos])) != 0) {
                ++endPos;
            }
            if (endPos < stripped.size() && stripped[endPos] == ';') {
                ++endPos;
            }
            while (endPos < stripped.size() &&
                std::isspace(static_cast<unsigned char>(stripped[endPos])) != 0) {
                ++endPos;
            }

            out.remaining += sourceText.substr(cursor, matchPos - cursor);
            std::string block = sourceText.substr(matchPos, endPos - matchPos);
            // ModuCPP allows omitting the trailing semicolon on an `enum class`
            // declaration; C++ requires it. Find the last non-whitespace character
            // in the extracted block and ensure a semicolon follows the closing brace.
            size_t lastNonSpace = block.find_last_not_of(" \t\r\n");
            if (lastNonSpace != std::string::npos && block[lastNonSpace] == '}') {
                block.insert(lastNonSpace + 1, ";");
            }
            out.extracted += block;
            if (!out.extracted.empty() && out.extracted.back() != '\n') {
                out.extracted.push_back('\n');
            }
            cursor = endPos;
        }

        out.remaining += sourceText.substr(cursor);
        return out;
    }

    std::string normalizeModuPackageLines(const std::string& sourceText) {
        std::istringstream input(sourceText);
        std::ostringstream out;
        std::string line;
        static const std::regex addPattern(R"(^(\s*)add\s+([A-Za-z_][A-Za-z0-9_:.]*)\s*;\s*$)");
        static const std::regex markPattern(R"(^(\s*)mark\s+([A-Za-z_][A-Za-z0-9_:]*)\s*;\s*$)");
        static const std::regex usingPattern(R"(^(\s*)using\s+([A-Za-z_][A-Za-z0-9_:.]*)\s*;\s*$)");
        static const std::unordered_set<std::string> packageNames = {
            "ModuCPP",
            "ModuEngine",
            "ModuInput",
            "ModuInput.XR",
            "ModuNetwork",
            "RMeshBuilder",
            "ModuCPP.Experimental",
        };

        while (std::getline(input, line)) {
            std::smatch match;
            if (std::regex_match(line, match, addPattern)) {
                const std::string indent = match[1].str();
                const std::string packageName = match[2].str();
                if (packageNames.find(packageName) != packageNames.end()) {
                    out << indent << "#include \"" << packageName << "\"\n";
                } else {
                    std::string namespaceName = packageName;
                    size_t dotPos = 0;
                    while ((dotPos = namespaceName.find('.', dotPos)) != std::string::npos) {
                        namespaceName.replace(dotPos, 1, "::");
                        dotPos += 2;
                    }
                    out << indent << "namespace " << namespaceName << " {}\n";
                    out << indent << "using namespace " << namespaceName << ";\n";
                }
                continue;
            }
            if (std::regex_match(line, match, markPattern)) {
                const std::string indent = match[1].str();
                const std::string namespaceName = match[2].str();
                out << indent << "namespace " << namespaceName << " {}\n";
                continue;
            }
            if (std::regex_match(line, match, usingPattern) &&
                trimCopy(line).rfind("using namespace", 0) != 0) {
                const std::string indent = match[1].str();
                const std::string packageName = match[2].str();
                out << indent << "using namespace " << packageName << ";\n";
                continue;
            }
            out << line << "\n";
        }
        return out.str();
    }

    std::string convertPublicEnumsToCpp(const std::string& sourceText) {
        const std::string stripped = stripCommentsPreserveLayout(sourceText);
        static const std::regex enumPattern(R"(\bpublic\s+enum\s+([A-Za-z_][A-Za-z0-9_]*)\b)");

        std::string out;
        size_t cursor = 0;
        auto begin = std::sregex_iterator(stripped.begin(), stripped.end(), enumPattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::smatch& match = *it;
            const size_t matchPos = static_cast<size_t>(match.position());
            const size_t matchLen = static_cast<size_t>(match.length());
            const std::string enumName = match[1].str();
            const size_t openBrace = stripped.find('{', matchPos + matchLen);
            if (openBrace == std::string::npos) {
                continue;
            }
            const size_t closeBrace = findMatchingBrace(stripped, openBrace);
            if (closeBrace == std::string::npos) {
                continue;
            }

            out += sourceText.substr(cursor, matchPos - cursor);
            out += "enum class " + enumName;
            out += sourceText.substr(matchPos + matchLen, openBrace - (matchPos + matchLen) + 1);
            out += sourceText.substr(openBrace + 1, closeBrace - openBrace - 1);
            out += "}";

            size_t next = closeBrace + 1;
            // Peek past whitespace to detect an explicit semicolon in the source.
            size_t semiCheck = next;
            while (semiCheck < sourceText.size() &&
                std::isspace(static_cast<unsigned char>(sourceText[semiCheck])) != 0) {
                ++semiCheck;
            }
            const bool hadExplicitSemi = (semiCheck < sourceText.size() && sourceText[semiCheck] == ';');
            // Emit the semicolon immediately after } so it does not end up on the
            // next line (which would prefix the following declaration with ";").
            out += ";";
            while (next < sourceText.size() &&
                std::isspace(static_cast<unsigned char>(sourceText[next])) != 0) {
                out.push_back(sourceText[next]);
                ++next;
            }
            if (hadExplicitSemi) {
                ++next; // consume the explicit semicolon already in the source
            }
            cursor = next;
        }
        out += sourceText.substr(cursor);
        return out;
    }

    bool subScriptLineNeedsSemicolon(const std::string& line) {
        const std::string trimmed = trimCopy(line);
        if (trimmed.empty()) {
            return false;
        }
        if (trimmed.rfind("//", 0) == 0 ||
            trimmed.rfind("/*", 0) == 0 ||
            trimmed.rfind("*", 0) == 0 ||
            trimmed.rfind("#", 0) == 0) {
            return false;
        }
        if (trimmed == "public:" || trimmed == "private:") {
            return false;
        }

        const char last = trimmed.back();
        return last != ';' && last != '{' && last != '}' && last != ':';
    }

    // Strip leading "public " / "private " from each line of a SubScript struct
    // body so the emitted C++ struct has plain member declarations. ModuCPP
    // SubScript fields commonly omit semicolons, but the passthrough C++ struct
    // still needs them.
    static std::string normalizeSubScriptStructBody(const std::string& body) {
        std::string result;
        result.reserve(body.size() + 16);
        size_t i = 0;
        while (i < body.size()) {
            const size_t lineStart = i;
            // Advance past leading whitespace on this line segment.
            while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) {
                ++i;
            }
            const size_t wsLen = i - lineStart;
            if (body.compare(i, 7, "public ") == 0) {
                result.append(body, lineStart, wsLen);
                i += 7; // skip "public "
            } else if (body.compare(i, 8, "private ") == 0) {
                result.append(body, lineStart, wsLen);
                i += 8; // skip "private "
            } else {
                result.append(body, lineStart, wsLen);
            }
            const size_t contentStart = result.size();
            // Copy the rest of the line up to, but not including, the newline.
            while (i < body.size() && body[i] != '\n') {
                result += body[i++];
            }
            const std::string normalizedLine = result.substr(contentStart);
            if (subScriptLineNeedsSemicolon(normalizedLine)) {
                result += ";";
            }
            if (i < body.size()) {
                result += body[i++]; // the '\n'
            }
        }
        return result;
    }

    std::string convertSubScriptsToCpp(const std::string& sourceText) {
        const std::string stripped = stripCommentsPreserveLayout(sourceText);
        static const std::regex subScriptPattern(R"(\bSubScript\s+([A-Za-z_][A-Za-z0-9_]*)\b)");

        std::string out;
        size_t cursor = 0;
        for (auto it = std::sregex_iterator(stripped.begin(), stripped.end(), subScriptPattern);
            it != std::sregex_iterator(); ++it) {
            const std::smatch& match = *it;
            const size_t matchPos = static_cast<size_t>(match.position());
            const size_t matchLen = static_cast<size_t>(match.length());

            const size_t openBrace = stripped.find('{', matchPos + matchLen);
            if (openBrace == std::string::npos) continue;
            const size_t closeBrace = findMatchingBrace(stripped, openBrace);
            if (closeBrace == std::string::npos) continue;

            // Everything before this SubScript declaration.
            out += sourceText.substr(cursor, matchPos - cursor);
            // "struct TypeName" instead of "SubScript TypeName".
            out += "struct " + match[1].str();
            // Spacing / anything between the name and the opening brace.
            out += sourceText.substr(matchPos + matchLen, openBrace - (matchPos + matchLen) + 1);
            // Struct body with "public "/"private " prefixes stripped and
            // ModuCPP-style implicit semicolons made explicit.
            const std::string bodyRaw = sourceText.substr(openBrace + 1, closeBrace - openBrace - 1);
            out += normalizeSubScriptStructBody(bodyRaw);
            // Closing brace with a mandatory semicolon (C++ structs require it).
            out += "}";
            size_t next = closeBrace + 1;
            size_t semiCheck = next;
            while (semiCheck < sourceText.size() &&
                std::isspace(static_cast<unsigned char>(sourceText[semiCheck])) != 0) {
                ++semiCheck;
            }
            const bool hadExplicitSemi = (semiCheck < sourceText.size() && sourceText[semiCheck] == ';');
            out += ";";
            while (next < sourceText.size() &&
                std::isspace(static_cast<unsigned char>(sourceText[next])) != 0) {
                out += sourceText[next++];
            }
            if (hadExplicitSemi) ++next;
            cursor = next;
        }
        out += sourceText.substr(cursor);
        return out;
    }

    std::string normalizeModuSource(const std::string& sourceText) {
        return convertSubScriptsToCpp(convertPublicEnumsToCpp(normalizeModuPackageLines(sourceText)));
    }

    static void splitCodeAndLiteralSpans(const std::string& text,
                                         std::vector<std::string>& spans,
                                         std::vector<bool>& spansAreCode) {
        spans.clear();
        spansAreCode.clear();

        std::string current;
        bool currentIsCode = true;
        auto flush = [&](bool nextIsCode) {
            if (!current.empty()) {
                spans.push_back(current);
                spansAreCode.push_back(currentIsCode);
                current.clear();
            }
            currentIsCode = nextIsCode;
        };

        size_t i = 0;
        const size_t n = text.size();
        while (i < n) {
            const char c = text[i];
            const char next = (i + 1 < n) ? text[i + 1] : '\0';

            if (c == '/' && next == '/') {
                flush(false);
                while (i < n && text[i] != '\n') current.push_back(text[i++]);
                flush(true);
                continue;
            }
            if (c == '/' && next == '*') {
                flush(false);
                current.push_back(text[i++]);
                current.push_back(text[i++]);
                while (i < n && !(text[i] == '*' && i + 1 < n && text[i + 1] == '/')) {
                    current.push_back(text[i++]);
                }
                if (i < n) current.push_back(text[i++]);
                if (i < n) current.push_back(text[i++]);
                flush(true);
                continue;
            }
            if (c == '"' || c == '\'') {
                const char quote = c;
                flush(false);
                current.push_back(text[i++]);
                while (i < n) {
                    if (text[i] == '\\' && i + 1 < n) {
                        current.push_back(text[i++]);
                        current.push_back(text[i++]);
                        continue;
                    }
                    if (text[i] == quote) {
                        current.push_back(text[i++]);
                        break;
                    }
                    if (text[i] == '\n') break;
                    current.push_back(text[i++]);
                }
                flush(true);
                continue;
            }

            current.push_back(text[i++]);
        }
        flush(true);
    }

    void replaceRegexAll(std::string& text, const std::regex& pattern, const std::string& replacement) {
        std::vector<std::string> spans;
        std::vector<bool> spansAreCode;
        splitCodeAndLiteralSpans(text, spans, spansAreCode);

        bool hasLiteral = false;
        for (size_t i = 0; i < spansAreCode.size(); ++i) {
            if (!spansAreCode[i]) { hasLiteral = true; break; }
        }

        if (hasLiteral) {
            std::string rebuiltSpans;
            rebuiltSpans.reserve(text.size() + 32);
            bool ok = true;
            for (size_t i = 0; i < spans.size(); ++i) {
                if (!spansAreCode[i]) {
                    rebuiltSpans += spans[i];
                    continue;
                }
                try {
                    rebuiltSpans += std::regex_replace(spans[i], pattern, replacement);
                } catch (const std::regex_error& e) {
                    if (e.code() != std::regex_constants::error_stack) throw;
                    ok = false;
                    break;
                }
            }
            if (ok) {
                text.swap(rebuiltSpans);
                return;
            }
        }

        try {
            text = std::regex_replace(text, pattern, replacement);
            return;
        } catch (const std::regex_error& e) {
            if (e.code() != std::regex_constants::error_stack) {
                throw;
            }
        }

        std::string rebuilt;
        rebuilt.reserve(text.size());

        constexpr size_t maxChunkSize = 4096;
        size_t chunkStart = 0;
        while (chunkStart < text.size()) {
            size_t chunkEnd = std::min(text.size(), chunkStart + maxChunkSize);
            if (chunkEnd < text.size()) {
                const size_t delimiter = text.find_last_of(";\n{}", chunkEnd);
                if (delimiter != std::string::npos && delimiter >= chunkStart) {
                    chunkEnd = delimiter + 1;
                }
            }

            const std::string chunk = text.substr(chunkStart, chunkEnd - chunkStart);
            rebuilt += std::regex_replace(chunk, pattern, replacement);
            chunkStart = chunkEnd;
        }

        text.swap(rebuilt);
    }

    std::string statementFromActionGroupItem(const std::string& item) {
        std::string stmt = trimCopy(item);
        if (stmt.empty()) return {};
        if (stmt.back() != ';') {
            stmt.push_back(';');
        }
        return stmt;
    }

    std::string rewriteThenSyntax(const std::string& sourceText) {
        std::string out;
        out.reserve(sourceText.size() + 32);

        enum class Mode {
            Normal,
            LineComment,
            BlockComment,
            StringLiteral,
            CharLiteral
        };

        Mode mode = Mode::Normal;
        bool escaped = false;
        size_t cursor = 0;
        size_t i = 0;
        while (i < sourceText.size()) {
            const char c = sourceText[i];
            const char next = (i + 1 < sourceText.size()) ? sourceText[i + 1] : '\0';

            if (mode == Mode::Normal) {
                if (c == '/' && next == '/') {
                    mode = Mode::LineComment;
                    i += 2;
                    continue;
                }
                if (c == '/' && next == '*') {
                    mode = Mode::BlockComment;
                    i += 2;
                    continue;
                }
                if (c == '"') {
                    mode = Mode::StringLiteral;
                    escaped = false;
                    ++i;
                    continue;
                }
                if (c == '\'') {
                    mode = Mode::CharLiteral;
                    escaped = false;
                    ++i;
                    continue;
                }

                if (sourceText.compare(i, 4, "then") == 0 &&
                    (i == 0 || !isIdentifierChar(sourceText[i - 1])) &&
                    (i + 4 >= sourceText.size() || !isIdentifierChar(sourceText[i + 4]))) {
                    out += sourceText.substr(cursor, i - cursor);
                    size_t afterThen = skipWhitespace(sourceText, i + 4);
                    if (afterThen < sourceText.size() && sourceText[afterThen] == '[') {
                        const size_t close = findMatchingBracket(sourceText, afterThen);
                        if (close != std::string::npos) {
                            out += "{ ";
                            const std::vector<std::string> items =
                                splitTopLevel(sourceText.substr(afterThen + 1, close - afterThen - 1), ',');
                            bool emittedAny = false;
                            for (const std::string& item : items) {
                                const std::string stmt = statementFromActionGroupItem(item);
                                if (stmt.empty()) continue;
                                if (emittedAny) out.push_back(' ');
                                out += stmt;
                                emittedAny = true;
                            }
                            out += " }";
                            size_t nextPos = close + 1;
                            nextPos = skipWhitespace(sourceText, nextPos);
                            if (nextPos < sourceText.size() && sourceText[nextPos] == ';') {
                                ++nextPos;
                            }
                            cursor = nextPos;
                            i = nextPos;
                            continue;
                        }
                    }

                    out.push_back(' ');
                    cursor = afterThen;
                    i = afterThen;
                    continue;
                }
            } else if (mode == Mode::LineComment) {
                if (c == '\n') {
                    mode = Mode::Normal;
                }
            } else if (mode == Mode::BlockComment) {
                if (c == '*' && next == '/') {
                    mode = Mode::Normal;
                    i += 2;
                    continue;
                }
            } else if (mode == Mode::StringLiteral) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    mode = Mode::Normal;
                }
            } else if (mode == Mode::CharLiteral) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '\'') {
                    mode = Mode::Normal;
                }
            }
            ++i;
        }

        out += sourceText.substr(cursor);
        return out;
    }

    std::string rewriteSurfaceSyntax(const std::string& sourceText) {
        std::string out = rewriteThenSyntax(sourceText);

        replaceRegexAll(out, std::regex(R"(\bMath\s*\.)"), "Math::");
        // Beginner-friendly namespaces that user scripts access with `.` even on
        // lowercase members (e.g. `Ensure.obj`). Convert ALL members of these
        replaceRegexAll(out, std::regex(R"(\bEnsure\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)\b)"), "Ensure::$1");
        replaceRegexAll(out, std::regex(R"(\bScene\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)\b)"), "Scene::$1");
        replaceRegexAll(out, std::regex(R"(\bMovement\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)\b)"), "Movement::$1");
        // Convert Namespace.Member → Namespace::Member for enum/namespace-style access.
        // Require the match not be preceded by '.' (i.e. not a chained member like obj.UI.Foo).
        // We capture an optional non-dot boundary character before the identifier.
        replaceRegexAll(out, std::regex(R"((^|[^.A-Za-z0-9_])([A-Z][A-Za-z0-9_]*)\s*\.\s*([A-Z][A-Za-z0-9_]*)\b)"),
                        "$1$2::$3");
        replaceRegexAll(out, std::regex(R"(\bModuEngine::FPS\b)"), "::ModuCPP::ModuEngine.FPS");
        replaceRegexAll(out, std::regex(R"(\bModuCPP::FPS\b)"), "::ModuCPP::ModuEngine.FPS");
        replaceRegexAll(out, std::regex(R"(\bInputXR::([A-Za-z_][A-Za-z0-9_]*)\b)"),
                        "::ModuCPP::InputXR.$1");
        replaceRegexAll(out, std::regex(R"(\bXRInteraction::([A-Za-z_][A-Za-z0-9_]*)\b)"),
                        "::ModuCPP::XRInteraction.$1");
        // Net is a facade object, not a namespace, but the Capital.Capital rule
        // above has already turned every `Net.Member` into `Net::Member`. Put it
        // back, the same way InputXR is restored just above. NetState and
        // AvatarSlot really are scoped and must NOT be caught here - they are not,
        // because \bNet:: requires "Net" immediately followed by "::".
        replaceRegexAll(out, std::regex(R"(\bNet::([A-Za-z_][A-Za-z0-9_]*)\b)"),
                        "::ModuCPP::Net.$1");
        // Fully qualify 'time.' to avoid ambiguity with the C standard library ::time() function
        // when 'using namespace ::ModuCPP' is in scope.
        replaceRegexAll(out, std::regex(R"(\btime\s*\.)"), "::ModuCPP::time.");
        replaceRegexAll(out, std::regex(R"(\bref\s+([A-Za-z_][A-Za-z0-9_]*)\b)"), "$1");
        replaceRegexAll(out, std::regex(R"((^|[^:A-Za-z0-9_])Vector2\b)"), "$1glm::vec2");
        replaceRegexAll(out, std::regex(R"((^|[^:A-Za-z0-9_])Vector3\b)"), "$1glm::vec3");
        replaceRegexAll(out, std::regex(R"((^|[^:A-Za-z0-9_])string\b)"), "$1std::string");
        replaceRegexAll(out,
                        std::regex(R"(\b([A-Za-z_][A-Za-z0-9_\[\]\(\)]*)\s*\.\s*Length\s*\(\s*\))"),
                        "glm::length($1)");
        replaceRegexAll(out,
                        std::regex(R"(\b([A-Za-z_][A-Za-z0-9_\[\]\(\)]*)\s*\.\s*Dot\s*\(\s*([^\)]+?)\s*\))"),
                        "glm::dot($1, $2)");
        replaceRegexAll(out,
                        std::regex(R"(\b([A-Za-z_][A-Za-z0-9_\[\]\(\)]*)\s*\.\s*IsEmpty\s*\(\s*\))"),
                        "$1.empty()");

        // Convert ModuCPP lambda syntax  () => { }  /  (Type param) => { }
        // to C++ capture-by-ref lambdas  [&]() { }  /  [&](Type param) { }
        // The prefix group (^|[^A-Za-z0-9_]) ensures we don't consume the opening
        // paren of the enclosing function call (e.g. OnValueChanged((float v) => )).
        replaceRegexAll(out,
                        std::regex(R"((^|[^A-Za-z0-9_])\(([^)]*)\)\s*=>)"),
                        "$1[&]($2)");

        std::string timerRewritten;
        timerRewritten.reserve(out.size() + 64);
        size_t timerCursor = 0;
        for (size_t i = 0; i < out.size();) {
            if (!isIdentifierStartChar(out[i])) {
                ++i;
                continue;
            }

            const size_t identStart = i;
            size_t identEnd = i + 1;
            while (identEnd < out.size() && isIdentifierChar(out[identEnd])) {
                ++identEnd;
            }

            size_t probe = skipWhitespace(out, identEnd);
            if (probe >= out.size() || out[probe] != '.') {
                i = identEnd;
                continue;
            }

            probe = skipWhitespace(out, probe + 1);
            std::string replacementPrefix;
            size_t methodEnd = probe;
            if (out.compare(probe, 5, "Start") == 0 &&
                (probe + 5 >= out.size() || !isIdentifierChar(out[probe + 5]))) {
                replacementPrefix = "::ModuCPP::StartTimer";
                methodEnd = probe + 5;
            } else if (out.compare(probe, 5, "Ready") == 0 &&
                    (probe + 5 >= out.size() || !isIdentifierChar(out[probe + 5]))) {
                replacementPrefix = "::ModuCPP::TimerReady";
                methodEnd = probe + 5;
            } else {
                i = identEnd;
                continue;
            }

            const size_t openParen = skipWhitespace(out, methodEnd);
            if (replacementPrefix == "::ModuCPP::TimerReady" &&
                (openParen >= out.size() || out[openParen] != '(')) {
                timerRewritten += out.substr(timerCursor, identStart - timerCursor);
                const std::string ident = trimCopy(out.substr(identStart, identEnd - identStart));
                timerRewritten += replacementPrefix;
                timerRewritten += "(";
                timerRewritten += ident;
                timerRewritten += ")";

                timerCursor = methodEnd;
                i = methodEnd;
                continue;
            }

            if (openParen >= out.size() || out[openParen] != '(') {
                i = identEnd;
                continue;
            }

            const size_t closeParen = findMatchingParen(out, openParen);
            if (closeParen == std::string::npos) {
                i = identEnd;
                continue;
            }

            timerRewritten += out.substr(timerCursor, identStart - timerCursor);
            const std::string ident = trimCopy(out.substr(identStart, identEnd - identStart));
            const std::string args = trimCopy(out.substr(openParen + 1, closeParen - openParen - 1));
            timerRewritten += replacementPrefix;
            timerRewritten += "(";
            timerRewritten += ident;
            if (!args.empty()) {
                timerRewritten += ", ";
                timerRewritten += args;
            }
            timerRewritten += ")";

            timerCursor = closeParen + 1;
            i = closeParen + 1;
        }
        if (timerCursor < out.size()) {
            timerRewritten += out.substr(timerCursor);
        }
        out.swap(timerRewritten);

        if (out.find('[') == std::string::npos) {
            return out;
        }

        static const std::regex arrayDeclPattern(
            R"((^|[;{}\n]\s*)([A-Za-z_][A-Za-z0-9_:.<>]*)\s*((?:\[[^\]]*\])+)\s+([A-Za-z_][A-Za-z0-9_]*))");
        std::string rebuilt;
        size_t cursor = 0;
        auto begin = std::sregex_iterator(out.begin(), out.end(), arrayDeclPattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::smatch& match = *it;
            const size_t pos = static_cast<size_t>(match.position());
            const size_t len = static_cast<size_t>(match.length());
            rebuilt += out.substr(cursor, pos - cursor);

            const std::string prefix = match[1].str();
            const std::string baseType = match[2].str();
            const std::string dimsText = match[3].str();
            const std::string name = match[4].str();

            std::vector<std::string> dims;
            size_t dimCursor = 0;
            while (dimCursor < dimsText.size()) {
                const size_t open = dimsText.find('[', dimCursor);
                const size_t close = dimsText.find(']', open == std::string::npos ? dimCursor : open);
                if (open == std::string::npos || close == std::string::npos) break;
                dims.push_back(trimCopy(dimsText.substr(open + 1, close - open - 1)));
                dimCursor = close + 1;
            }

            std::string cppType = mapScriptBaseTypeToCpp(baseType);
            for (auto dimIt = dims.rbegin(); dimIt != dims.rend(); ++dimIt) {
                if (trimCopy(*dimIt).empty()) {
                    cppType = "std::vector<" + cppType + ">";
                } else {
                    cppType = "std::array<" + cppType + ", " + *dimIt + ">";
                }
            }

            rebuilt += prefix + cppType + " " + name;
            cursor = pos + len;
        }
        rebuilt += out.substr(cursor);
        return rebuilt;
    }

    bool parseClass(const std::string& sourceText, ClassSpec& outClass, std::string& error) {
        std::istringstream input(sourceText);
        std::string line;
        while (std::getline(input, line)) {
            std::string trimmed = trimCopy(line);
            if (trimmed.rfind("#include", 0) == 0) {
                outClass.includeDirectives.push_back(trimmed);
            }
        }

        const std::string stripped = stripCommentsPreserveLayout(sourceText);
        const std::optional<ModuClassMatch> classMatch = findModuClassDeclaration(stripped);
        if (!classMatch) {
            error = "No ModuCPP class found. Expected: public class <Name> : ModuNode or ModuBehaviour";
            return false;
        }

        outClass.name = classMatch->name;
        const size_t classDeclPos = classMatch->position;
        const size_t classDeclEnd = classMatch->end;
        const size_t classOpenBrace = stripped.find('{', classDeclEnd);
        if (classOpenBrace == std::string::npos) {
            error = "ModuCPP class body is missing '{'.";
            return false;
        }
        const size_t classCloseBrace = findMatchingBrace(stripped, classOpenBrace);
        if (classCloseBrace == std::string::npos) {
            error = "ModuCPP class body has unmatched '{'.";
            return false;
        }

        size_t classEnd = classCloseBrace + 1;
        while (classEnd < stripped.size() &&
            std::isspace(static_cast<unsigned char>(stripped[classEnd])) != 0) {
            ++classEnd;
        }
        if (classEnd < stripped.size() && stripped[classEnd] == ';') {
            ++classEnd;
        }
        while (classEnd < stripped.size() &&
            std::isspace(static_cast<unsigned char>(stripped[classEnd])) != 0) {
            ++classEnd;
        }

        if (classDeclPos <= sourceText.size()) {
            outClass.passthroughCode += sourceText.substr(0, classDeclPos);
        }
        if (classEnd <= sourceText.size()) {
            outClass.passthroughCode += sourceText.substr(classEnd);
        }
        outClass.subScripts = parseSubScriptStructs(outClass.passthroughCode);

        const size_t bodyStart = classOpenBrace + 1;
        const size_t bodyLen = classCloseBrace - bodyStart;
        const std::string bodyRaw = sourceText.substr(bodyStart, bodyLen);
        const std::string bodyClean = stripped.substr(bodyStart, bodyLen);

        std::set<std::string> fieldNames;
        size_t i = 0;
        while (i < bodyClean.size()) {
            while (i < bodyClean.size() && std::isspace(static_cast<unsigned char>(bodyClean[i])) != 0) {
                ++i;
            }
            if (i >= bodyClean.size()) break;

            size_t nextSemicolon = std::string::npos;
            size_t nextOpenBrace = std::string::npos;
            int parenDepth = 0;
            int angleDepth = 0;
            int bracketDepth = 0;
            int braceDepth = 0;
            bool sawParenInSignature = false;
            bool inString = false;
            bool inChar = false;
            bool escaped = false;

            for (size_t j = i; j < bodyClean.size(); ++j) {
                const char c = bodyClean[j];
                if (inString) {
                    if (escaped) {
                        escaped = false;
                    } else if (c == '\\') {
                        escaped = true;
                    } else if (c == '"') {
                        inString = false;
                    }
                    continue;
                }
                if (inChar) {
                    if (escaped) {
                        escaped = false;
                    } else if (c == '\\') {
                        escaped = true;
                    } else if (c == '\'') {
                        inChar = false;
                    }
                    continue;
                }

                if (c == '"') {
                    inString = true;
                    escaped = false;
                    continue;
                }
                if (c == '\'') {
                    inChar = true;
                    escaped = false;
                    continue;
                }

                if (c == '(') {
                    if (parenDepth == 0 && angleDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
                        sawParenInSignature = true;
                    }
                    ++parenDepth;
                }
                else if (c == ')') {
                    parenDepth = std::max(0, parenDepth - 1);
                    // Once the parameter list closes, any subsequent '<'/'>' are comparison
                    // operators (not template brackets), so reset angle depth to avoid
                    // mis-tracking expressions like 'clip < ctx.GetSpriteClipCount()'.
                    if (sawParenInSignature && parenDepth == 0 && braceDepth == 0) {
                        angleDepth = 0;
                    }
                }
                else if (c == '<') ++angleDepth;
                else if (c == '>') angleDepth = std::max(0, angleDepth - 1);
                else if (c == '[') ++bracketDepth;
                else if (c == ']') bracketDepth = std::max(0, bracketDepth - 1);
                else if (c == '{') {
                    if (parenDepth == 0 && angleDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
                        const std::string signatureProbe = trimCopy(bodyRaw.substr(i, j - i));
                        if (signatureProbe == "inspector" || sawParenInSignature) {
                            nextOpenBrace = j;
                            break;
                        }
                    }
                    ++braceDepth;
                    continue;
                } else if (c == '}') {
                    braceDepth = std::max(0, braceDepth - 1);
                }

                if (parenDepth == 0 && angleDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
                    if (c == ';') {
                        nextSemicolon = j;
                        break;
                    }
                }
            }

            if (nextSemicolon == std::string::npos && nextOpenBrace == std::string::npos) {
                break;
            }

            if (nextOpenBrace != std::string::npos &&
                (nextSemicolon == std::string::npos || nextOpenBrace < nextSemicolon)) {
                const std::string signatureDecl = bodyRaw.substr(i, nextOpenBrace - i);
                if (std::optional<size_t> missingSemicolonOffset =
                        findLikelyMissingSemicolonBeforeMethod(signatureDecl)) {
                    error = formatLocatedParseError(
                        "Missing ';'",
                        sourceText,
                        bodyStart + i + *missingSemicolonOffset);
                    return false;
                }

                const size_t methodCloseBrace = findMatchingBrace(bodyClean, nextOpenBrace);
                if (methodCloseBrace == std::string::npos) {
                    error = "Method body has unmatched '{' in class: " + outClass.name;
                    return false;
                }

                std::string methodBody = bodyRaw.substr(nextOpenBrace + 1, methodCloseBrace - nextOpenBrace - 1);
                const std::string signatureTrimmed = trimCopy(signatureDecl);
                if (signatureTrimmed == "inspector") {
                    if (!trimCopy(outClass.inspectorBlock).empty()) {
                        error = "Only one 'inspector { ... }' block is allowed per ModuCPP class.";
                        return false;
                    }
                    outClass.inspectorBlock = methodBody;
                    i = methodCloseBrace + 1;
                    if (i < bodyClean.size() && bodyClean[i] == ';') {
                        ++i;
                    }
                    continue;
                }

                MethodSpec method;
                const int methodBodySourceLine = lineNumberForOffset(sourceText, bodyStart + nextOpenBrace + 1);
                if (!parseMethodDecl(signatureDecl, methodBody, methodBodySourceLine, method, error)) {
                    return false;
                }
                outClass.methods.push_back(std::move(method));

                i = methodCloseBrace + 1;
                if (i < bodyClean.size() && bodyClean[i] == ';') {
                    ++i;
                }
                continue;
            }

            std::string statementDecl = bodyRaw.substr(i, nextSemicolon - i + 1);
            const std::string statementTrimmed = trimCopy(statementDecl);
            const size_t statementParen = findTopLevelChar(statementTrimmed, '(');
            if (statementParen != std::string::npos) {
                const size_t statementAssign = findTopLevelChar(statementTrimmed, '=');
                const bool looksLikeFieldInitializer =
                    statementAssign != std::string::npos && statementAssign < statementParen;
                MethodSpec method;
                std::string methodSignature = statementTrimmed;
                if (!methodSignature.empty() && methodSignature.back() == ';') {
                    methodSignature.pop_back();
                    methodSignature = trimCopy(methodSignature);
                }
                if (!looksLikeFieldInitializer) {
                    std::string methodError;
                    const int methodBodySourceLine = lineNumberForOffset(sourceText, bodyStart + i);
                    if (parseMethodDecl(methodSignature, std::string(), methodBodySourceLine, method, methodError)) {
                        outClass.methods.push_back(std::move(method));
                        i = nextSemicolon + 1;
                        continue;
                    }
                }
            }

            if (std::optional<size_t> missingSemicolonOffset = findLikelyMissingSemicolonInFieldDecl(statementDecl)) {
                error = formatLocatedParseError(
                    "Missing ';'",
                    sourceText,
                    bodyStart + i + *missingSemicolonOffset);
                return false;
            }
            FieldSpec field;
            if (parseFieldDecl(statementDecl, field, error)) {
                if (!fieldNames.insert(field.name).second) {
                    error = "Duplicate field name in ModuCPP class: " + field.name;
                    return false;
                }
                outClass.fields.push_back(std::move(field));
            } else if (!error.empty()) {
                return false;
            }
            i = nextSemicolon + 1;
        }

        bool hasInspectorDrivenField = false;
        for (const FieldSpec& field : outClass.fields) {
            if (field.visibility == FieldVisibility::Public || field.hasInspectorMetadata) {
                hasInspectorDrivenField = true;
                break;
            }
        }
        if (outClass.methods.empty() && trimCopy(outClass.inspectorBlock).empty() && !hasInspectorDrivenField) {
            error = "ModuCPP class has no methods to transpile.";
            return false;
        }
        return true;
    }

    std::string generateTranspiledSource(const fs::path& sourcePath, const ClassSpec& spec,
                                        std::string& error) {
        const std::string supportNs = "ModuCPPTranspiled_" + sanitizeIdentifier(spec.name);
        const std::string configType = spec.name + "Config";
        const std::string stateType = spec.name + "State";
        const std::string strippedInspector = stripCommentsPreserveLayout(spec.inspectorBlock);
        std::unordered_set<std::string> clipGridFollowerFieldNames;
        std::unordered_map<std::string, const SubScriptSpec*> subScriptByType;
        for (const SubScriptSpec& subScript : spec.subScripts) {
            subScriptByType.emplace(toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(subScript.name))), &subScript);
        }

        auto findSubScriptByType = [&](const std::string& typeName) -> const SubScriptSpec* {
            const auto it = subScriptByType.find(toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(typeName))));
            return it == subScriptByType.end() ? nullptr : it->second;
        };

        auto fieldUsesSupportedSubScript = [&](const FieldSpec& field) -> bool {
            if (field.kind != FieldKind::Custom) {
                return false;
            }
            if (findSubScriptByType(field.baseType) == nullptr) {
                return false;
            }
            if (field.arrayDimensions.empty()) {
                return true;
            }
            return field.arrayDimensions.size() == 1 && trimCopy(field.arrayDimensions[0]).empty();
        };

        std::unordered_set<std::string> reachableSubScriptTypes;
        std::function<void(const std::string&)> markReachableSubScript = [&](const std::string& typeName) {
            const SubScriptSpec* subScript = findSubScriptByType(typeName);
            if (!subScript) {
                return;
            }
            const std::string normalized = toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(subScript->name)));
            if (!reachableSubScriptTypes.insert(normalized).second) {
                return;
            }
            for (const FieldSpec& nestedField : subScript->fields) {
                markReachableSubScript(nestedField.baseType);
            }
        };

        auto fieldPersists = [&](const FieldSpec& field) {
            return field.visibility == FieldVisibility::Public ||
                field.hasInspectorMetadata ||
                clipGridFollowerFieldNames.find(field.name) != clipGridFollowerFieldNames.end();
        };

        std::unordered_set<std::string> listFields;
        std::unordered_set<std::string> objectRefFields;
        std::vector<std::string> customPublicFieldNames;
        bool hasAutoInspectorFields = false;
        bool needsDialogueLinesSupport = strippedInspector.find("DialogueLines") != std::string::npos;
        bool needsDialoguePortSupport = needsDialogueLinesSupport;
        bool needsObjectRefSupport = false;
        bool needsObjectListSupport = false;
        bool hasTransientFields = false;
        auto fieldNeedsDialogueLinesSupport = [&](const FieldSpec& field) {
            if (field.kind == FieldKind::DialogueLines || field.hasDialogueLinesAttribute) {
                return true;
            }
            if (field.arrayDimensions.size() != 1 || !trimCopy(field.arrayDimensions[0]).empty()) {
                return false;
            }
            return toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(field.baseType))) == "dialogueport::dialogueline";
        };
        for (const FieldSpec& field : spec.fields) {
            if (!fieldPersists(field)) {
                hasTransientFields = true;
            }
            if (fieldPersists(field) && field.kind == FieldKind::ObjectList) {
                listFields.insert(field.name);
            }
            if (fieldPersists(field) &&
                (field.kind == FieldKind::ObjectRef || field.hasObjectRefAttribute)) {
                objectRefFields.insert(field.name);
            }
            if (field.kind == FieldKind::ObjectList || field.hasObjectListAttribute) {
                needsObjectListSupport = true;
            }
            if (field.kind == FieldKind::ObjectRef ||
                field.kind == FieldKind::ObjectList ||
                field.hasObjectRefAttribute ||
                field.hasObjectListAttribute) {
                needsObjectRefSupport = true;
            }
            if (field.kind == FieldKind::DialogueLines ||
                field.hasDialogueLinesAttribute) {
                needsDialoguePortSupport = true;
            }
            if (fieldNeedsDialogueLinesSupport(field)) {
                needsDialogueLinesSupport = true;
                needsDialoguePortSupport = true;
            }
            if (field.visibility == FieldVisibility::Public || field.hasInspectorMetadata) {
                hasAutoInspectorFields = true;
            }
            if (field.visibility == FieldVisibility::Public &&
                field.kind == FieldKind::Custom &&
                !fieldUsesSupportedSubScript(field)) {
                customPublicFieldNames.push_back(field.name + " : " + field.rawType);
            }
            if (fieldPersists(field) && fieldUsesSupportedSubScript(field)) {
                markReachableSubScript(field.baseType);
            }
        }
        for (const SubScriptSpec& subScript : spec.subScripts) {
            for (const FieldSpec& field : subScript.fields) {
                if (field.kind == FieldKind::ObjectList || field.hasObjectListAttribute) {
                    needsObjectListSupport = true;
                }
                if (field.kind == FieldKind::ObjectRef ||
                    field.kind == FieldKind::ObjectList ||
                    field.hasObjectRefAttribute ||
                    field.hasObjectListAttribute) {
                    needsObjectRefSupport = true;
                }
                if (field.kind == FieldKind::DialogueLines ||
                    field.hasDialogueLinesAttribute) {
                    needsDialoguePortSupport = true;
                }
                if (fieldNeedsDialogueLinesSupport(field)) {
                    needsDialogueLinesSupport = true;
                    needsDialoguePortSupport = true;
                }
            }
        }

        const bool hasReachableSubScripts = std::any_of(
            spec.subScripts.begin(), spec.subScripts.end(),
            [&](const SubScriptSpec& ss) {
                return reachableSubScriptTypes.count(
                        toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(ss.name)))) > 0;
            });
        const bool needsEscapedStringHelpers = needsObjectListSupport || hasReachableSubScripts;

        std::ostringstream out;
        std::unordered_map<std::string, std::string> rewrittenSyntaxCache;
        std::unordered_map<std::string, std::string> mappedTypeCache;

        auto cachedRewriteSurfaceSyntax = [&](const std::string& text) -> const std::string& {
            const auto it = rewrittenSyntaxCache.find(text);
            if (it != rewrittenSyntaxCache.end()) {
                return it->second;
            }
            return rewrittenSyntaxCache.emplace(text, rewriteSurfaceSyntax(text)).first->second;
        };

        auto cachedMapScriptTypeToCpp = [&](const std::string& typeName) -> const std::string& {
            const auto it = mappedTypeCache.find(typeName);
            if (it != mappedTypeCache.end()) {
                return it->second;
            }
            return mappedTypeCache.emplace(typeName, mapScriptTypeToCpp(typeName)).first->second;
        };

        auto emitSourceLineDirective = [&](int line) {
            if (line <= 0) {
                return;
            }
            out << "#line " << line << " \"" << escapeCStringLiteral(sourcePath.lexically_normal().generic_string()) << "\"\n";
        };

        auto emitGeneratedLineDirective = [&]() {
            out << "#line 1 \"" << escapeCStringLiteral(sourcePath.filename().string() + ".gen.cpp") << "\"\n";
        };

        auto findDirectionalClipWalkField = [&](size_t idleFieldIndex) -> const FieldSpec* {
            for (size_t j = idleFieldIndex + 1; j < spec.fields.size(); ++j) {
                const FieldSpec& candidate = spec.fields[j];
                if (candidate.arrayDimensions.size() == 2 &&
                    candidate.arrayDimensions[0] == "4" &&
                    candidate.arrayDimensions[1] == "4" &&
                    toLowerCopy(removeWhitespaceCopy(candidate.baseType)) == "int") {
                    return &candidate;
                }
            }
            return nullptr;
        };

        for (size_t i = 0; i < spec.fields.size(); ++i) {
            const FieldSpec& field = spec.fields[i];
            if (!field.hasClipGridPairAttribute) {
                continue;
            }
            const FieldSpec* walkField = findDirectionalClipWalkField(i);
            if (walkField) {
                clipGridFollowerFieldNames.insert(walkField->name);
            }
        }

        auto emitAutoInspectorField = [&](size_t fieldIndex, const FieldSpec& field,
                                        std::unordered_set<std::string>& skippedFieldNames) -> bool {
            if (!fieldPersists(field) || skippedFieldNames.find(field.name) != skippedFieldNames.end()) {
                return true;
            }

            const std::string indent = "    ";
            const std::string fieldAccess = "config." + field.name;
            const std::string label = inspectorLabelFromFieldName(field.name);

            if (field.inspectorHeader.has_value()) {
                out << indent << "ModuGUI::TextUnformatted(" << *field.inspectorHeader << ");\n";
                out << indent << "ModuGUI::Separator();\n";
            }
            if (field.hasSeparatorAttribute) {
                out << indent << "ModuGUI::Separator();\n";
            }

            if (field.hasClipGridPairAttribute) {
                if (!(field.arrayDimensions.size() == 1 && field.arrayDimensions[0] == "4" &&
                    toLowerCopy(removeWhitespaceCopy(field.baseType)) == "int")) {
                    error = "[ClipGridPair] field '" + field.name + "' must be an int[4] array.";
                    return false;
                }
                const FieldSpec* walkField = findDirectionalClipWalkField(fieldIndex);
                if (!walkField) {
                    error = "[ClipGridPair] field '" + field.name + "' requires a following int[4][4] field.";
                    return false;
                }
                out << indent << "changed |= EditDirectionalClipGrid(" << fieldAccess
                    << ", config." << walkField->name << ");\n";
                skippedFieldNames.insert(walkField->name);
                return true;
            }

            if (field.soundSetLabel.has_value()) {
                if (field.arrayDimensions.empty() || toLowerCopy(removeWhitespaceCopy(field.baseType)) != "string") {
                    error = "[SoundSet] field '" + field.name + "' must use a string array type.";
                    return false;
                }
                out << indent << "changed |= EditSoundSet(" << *field.soundSetLabel
                    << ", " << fieldAccess << ");\n";
                return true;
            }

            if (field.hasSliderAttribute &&
                field.kind != FieldKind::Float &&
                field.kind != FieldKind::Int &&
                field.kind != FieldKind::Vec3) {
                error = "[Slider] field '" + field.name + "' must be a float, int, or vec3.";
                return false;
            }

            if (field.kind == FieldKind::Float) {
                if (field.hasSliderAttribute && field.sliderMinExpr.has_value() && field.sliderMaxExpr.has_value()) {
                    out << indent << "changed |= ModuGUI::SliderFloat(\"" << escapeCStringLiteral(label)
                        << "\", &" << fieldAccess << ", " << *field.sliderMinExpr
                        << ", " << *field.sliderMaxExpr << ");\n";
                } else {
                    out << indent << "changed |= ModuGUI::DragFloat(\"" << escapeCStringLiteral(label)
                        << "\", &" << fieldAccess << ", " << field.stepExpr.value_or("0.01f")
                        << ", " << field.rangeMinExpr.value_or("0.0f")
                        << ", " << field.rangeMaxExpr.value_or("0.0f") << ");\n";
                }
                return true;
            }
            if (field.kind == FieldKind::Int) {
                if (field.hasSliderAttribute && field.sliderMinExpr.has_value() && field.sliderMaxExpr.has_value()) {
                    out << indent << "changed |= ModuGUI::SliderInt(\"" << escapeCStringLiteral(label)
                        << "\", &" << fieldAccess << ", static_cast<int>(" << *field.sliderMinExpr
                        << "), static_cast<int>(" << *field.sliderMaxExpr << "));\n";
                } else if (field.rangeMinExpr.has_value() && field.rangeMaxExpr.has_value()) {
                    out << indent << "changed |= ModuGUI::SliderInt(\"" << escapeCStringLiteral(label)
                        << "\", &" << fieldAccess << ", static_cast<int>(" << field.rangeMinExpr.value()
                        << "), static_cast<int>(" << field.rangeMaxExpr.value() << "));\n";
                } else {
                    out << indent << "changed |= ModuGUI::InputInt(\"" << escapeCStringLiteral(label)
                        << "\", &" << fieldAccess << ");\n";
                }
                return true;
            }
            if (field.kind == FieldKind::Bool) {
                out << indent << "changed |= ModuGUI::Checkbox(\"" << escapeCStringLiteral(label)
                    << "\", &" << fieldAccess << ");\n";
                return true;
            }
            if (field.kind == FieldKind::Vec3) {
                if (field.hasSliderAttribute && field.sliderMinExpr.has_value() && field.sliderMaxExpr.has_value()) {
                    out << indent << "changed |= ModuGUI::SliderFloat3(\"" << escapeCStringLiteral(label)
                        << "\", &" << fieldAccess << ".x, " << *field.sliderMinExpr
                        << ", " << *field.sliderMaxExpr << ", \"%.3f\");\n";
                } else {
                    out << indent << "changed |= ModuGUI::DragFloat3(\"" << escapeCStringLiteral(label)
                        << "\", &" << fieldAccess << ".x, " << field.stepExpr.value_or("0.01f")
                        << ", " << field.rangeMinExpr.value_or("-1000.0f")
                        << ", " << field.rangeMaxExpr.value_or("1000.0f")
                        << ", \"%.3f\");\n";
                }
                return true;
            }
            if (field.kind == FieldKind::String || field.kind == FieldKind::ObjectRef) {
                if (field.kind == FieldKind::ObjectRef || field.hasObjectRefAttribute) {
                    const std::string changedVar = "_moduChangedRef_" + std::to_string(fieldIndex);
                    out << indent << "{\n";
                    out << indent << "    const bool " << changedVar
                        << " = DrawObjectRefInput(ctx, \"" << escapeCStringLiteral(label)
                        << "\", " << fieldAccess << ");\n";
                    out << indent << "    changed |= " << changedVar << ";\n";
                    out << indent << "    if (" << changedVar << ") {\n";
                    out << indent << "        ctx.SetSetting(\"" << field.name
                        << "\", " << fieldAccess << ");\n";
                    out << indent << "    }\n";
                    out << indent << "}\n";
                } else {
                    out << indent << "{\n";
                    out << indent << "    std::vector<char> buffer(512, '\\0');\n";
                    out << indent << "    std::snprintf(buffer.data(), buffer.size(), \"%s\", " << fieldAccess << ".c_str());\n";
                    out << indent << "    if (ModuGUI::InputText(\"" << escapeCStringLiteral(label)
                        << "\", buffer.data(), buffer.size())) {\n";
                    out << indent << "        " << fieldAccess << " = buffer.data();\n";
                    out << indent << "        changed = true;\n";
                    out << indent << "    }\n";
                    out << indent << "}\n";
                }
                return true;
            }
            if (field.kind == FieldKind::ObjectList) {
                const std::string changedVar = "_moduChanged_" + std::to_string(fieldIndex);
                out << indent << "{\n";
                out << indent << "    const bool " << changedVar << " = " << supportNs << "::DrawObjectRefListEditor(ctx, \""
                    << escapeCStringLiteral(label) << "\", " << fieldAccess << ");\n";
                out << indent << "    changed |= " << changedVar << ";\n";
                out << indent << "    if (" << changedVar << ") {\n";
                out << indent << "        ctx.SetSetting(\"" << field.name
                    << "\", " << supportNs << "::SerializeObjectRefs(" << fieldAccess << "));\n";
                out << indent << "    }\n";
                out << indent << "}\n";
                return true;
            }
            if (field.kind == FieldKind::DialogueLines) {
                const std::string changedVar = "_moduChanged_" + std::to_string(fieldIndex);
                out << indent << "{\n";
                out << indent << "    const bool " << changedVar << " = " << supportNs
                    << "::DrawDialogueLinesEditor(ctx, "
                    << fieldAccess << ");\n";
                out << indent << "    changed |= " << changedVar << ";\n";
                out << indent << "    if (" << changedVar << ") {\n";
                out << indent << "        ctx.SetSetting(\"" << field.name
                    << "\", DialoguePort::SerializeDialogueLines(" << fieldAccess << "));\n";
                out << indent << "    }\n";
                out << indent << "}\n";
                return true;
            }

            if (findSubScriptByType(field.baseType) != nullptr) {
                const std::string changedVar = "_moduChanged_" + std::to_string(fieldIndex);
                out << indent << "{\n";
                if (field.arrayDimensions.empty()) {
                    out << indent << "    const bool " << changedVar << " = ::ModuCPP::EditSubScript(\""
                        << escapeCStringLiteral(label) << "\", " << fieldAccess << ");\n";
                    out << indent << "    changed |= " << changedVar << ";\n";
                    out << indent << "    if (" << changedVar << ") {\n";
                    out << indent << "        ctx.SetSetting(\"" << field.name
                        << "\", ::ModuCPP::SerializeSubScript(" << fieldAccess << "));\n";
                    out << indent << "    }\n";
                } else if (field.arrayDimensions.size() == 1 && trimCopy(field.arrayDimensions[0]).empty()) {
                    out << indent << "    const bool " << changedVar << " = ::ModuCPP::EditSubScriptArray(\""
                        << escapeCStringLiteral(label) << "\", " << fieldAccess << ");\n";
                    out << indent << "    changed |= " << changedVar << ";\n";
                    out << indent << "    if (" << changedVar << ") {\n";
                    out << indent << "        ctx.SetSetting(\"" << field.name
                        << "\", ::ModuCPP::SerializeSubScriptArray(" << fieldAccess << "));\n";
                    out << indent << "    }\n";
                } else {
                    error = "SubScript field '" + field.name + "' only supports singular values or one-dimensional dynamic arrays.";
                    return false;
                }
                out << indent << "}\n";
                return true;
            }

            return true;
        };

        auto fieldLooksLikeDialogueLineArray = [&](const FieldSpec& field) {
            return field.arrayDimensions.size() == 1 &&
                trimCopy(field.arrayDimensions[0]).empty() &&
                toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(field.baseType))) ==
                    "dialogueport::dialogueline";
        };

        auto fieldLooksLikeStringArray = [&](const FieldSpec& field) {
            return field.arrayDimensions.size() == 1 &&
                trimCopy(field.arrayDimensions[0]).empty() &&
                toLowerCopy(removeWhitespaceCopy(field.baseType)) == "string";
        };

        auto fieldLooksLikeObjectRefString = [&](const FieldSpec& field) {
            if (toLowerCopy(removeWhitespaceCopy(field.baseType)) != "string" || !field.arrayDimensions.empty()) {
                return false;
            }
            const std::string lowerName = toLowerCopy(field.name);
            return lowerName.find("ref") != std::string::npos;
        };

        auto subScriptFieldSerializeExpr = [&](const FieldSpec& field, const std::string& valueExpr)
            -> std::optional<std::string> {
            if (field.kind == FieldKind::Float || field.kind == FieldKind::Int) {
                return "std::to_string(" + valueExpr + ")";
            }
            if (field.kind == FieldKind::Bool) {
                return "(" + valueExpr + " ? \"1\" : \"0\")";
            }
            if (field.kind == FieldKind::String || field.kind == FieldKind::ObjectRef ||
                fieldLooksLikeObjectRefString(field)) {
                return valueExpr;
            }
            if (field.kind == FieldKind::Vec3) {
                return "std::to_string(" + valueExpr + ".x) + \",\" + std::to_string(" + valueExpr +
                    ".y) + \",\" + std::to_string(" + valueExpr + ".z)";
            }
            if (fieldLooksLikeStringArray(field) || field.kind == FieldKind::ObjectList) {
                return supportNs + "::SerializeObjectRefs(" + valueExpr + ")";
            }
            if (fieldLooksLikeDialogueLineArray(field) || field.kind == FieldKind::DialogueLines) {
                return "DialoguePort::SerializeDialogueLines(" + valueExpr + ")";
            }
            if (findSubScriptByType(field.baseType) != nullptr) {
                if (field.arrayDimensions.empty()) {
                    return "::ModuCPP::SerializeSubScript(" + valueExpr + ")";
                }
                if (field.arrayDimensions.size() == 1 && trimCopy(field.arrayDimensions[0]).empty()) {
                    return "::ModuCPP::SerializeSubScriptArray(" + valueExpr + ")";
                }
            }
            if (field.arrayDimensions.empty()) {
                return "std::to_string(static_cast<int>(" + valueExpr + "))";
            }
            return std::nullopt;
        };

        auto emitSubScriptFieldDeserialize = [&](size_t fieldIndex,
                                                const FieldSpec& field,
                                                const std::string& encodedExpr,
                                                const std::string& valueExpr) -> bool {
            if (field.kind == FieldKind::Float) {
                out << "        " << valueExpr << " = fields.size() > " << fieldIndex << " ? std::strtof(("
                    << encodedExpr << ").c_str(), nullptr) : " << valueExpr << ";\n";
                return true;
            }
            if (field.kind == FieldKind::Int) {
                out << "        if (fields.size() > " << fieldIndex << ") " << valueExpr
                    << " = std::atoi((" << encodedExpr << ").c_str());\n";
                return true;
            }
            if (field.kind == FieldKind::Bool) {
                out << "        if (fields.size() > " << fieldIndex << ") " << valueExpr
                    << " = (Trim(" << encodedExpr << ") == \"1\" || Trim(" << encodedExpr << ") == \"true\");\n";
                return true;
            }
            if (field.kind == FieldKind::String || field.kind == FieldKind::ObjectRef ||
                fieldLooksLikeObjectRefString(field)) {
                out << "        if (fields.size() > " << fieldIndex << ") " << valueExpr << " = " << encodedExpr << ";\n";
                return true;
            }
            if (field.kind == FieldKind::Vec3) {
                out << "        if (fields.size() > " << fieldIndex << ") std::sscanf((" << encodedExpr
                    << ").c_str(), \"%f,%f,%f\", &" << valueExpr << ".x, &" << valueExpr
                    << ".y, &" << valueExpr << ".z);\n";
                return true;
            }
            if (fieldLooksLikeStringArray(field) || field.kind == FieldKind::ObjectList) {
                out << "        if (fields.size() > " << fieldIndex << ") " << valueExpr
                    << " = " << supportNs << "::DeserializeObjectRefs(" << encodedExpr << ");\n";
                return true;
            }
            if (fieldLooksLikeDialogueLineArray(field) || field.kind == FieldKind::DialogueLines) {
                out << "        if (fields.size() > " << fieldIndex << ") " << valueExpr
                    << " = DialoguePort::DeserializeDialogueLines(" << encodedExpr << ");\n";
                return true;
            }
            if (const SubScriptSpec* subScript = findSubScriptByType(field.baseType)) {
                if (field.arrayDimensions.empty()) {
                    out << "        if (fields.size() > " << fieldIndex << ") " << valueExpr
                            << " = ::ModuCPP::DeserializeSubScript<" << mapScriptBaseTypeToCpp(subScript->name)
                        << ">(" << encodedExpr << ");\n";
                    return true;
                }
                if (field.arrayDimensions.size() == 1 && trimCopy(field.arrayDimensions[0]).empty()) {
                    out << "        if (fields.size() > " << fieldIndex << ") " << valueExpr
                        << " = ::ModuCPP::DeserializeSubScriptArray<" << mapScriptBaseTypeToCpp(subScript->name)
                        << ">(" << encodedExpr << ");\n";
                    return true;
                }
            }
            if (field.arrayDimensions.empty()) {
                    out << "        if (fields.size() > " << fieldIndex << ") " << valueExpr
                    << " = static_cast<" << cachedMapScriptTypeToCpp(field.rawType) << ">(std::atoi(("
                    << encodedExpr << ").c_str()));\n";
                return true;
            }
            error = "Unsupported SubScript field deserialization for '" + field.name + "' in " + spec.name + ".";
            return false;
        };

        auto emitSubScriptFieldEdit = [&](const FieldSpec& field,
                                        const std::string& labelLiteral,
                                        const std::string& valueExpr) -> bool {
            if (field.kind == FieldKind::Float) {
                out << "        changed |= ModuGUI::DragFloat(" << labelLiteral << ", &" << valueExpr
                    << ", 0.01f, 0.0f, 0.0f);\n";
                return true;
            }
            if (field.kind == FieldKind::Int) {
                out << "        changed |= ModuGUI::InputInt(" << labelLiteral << ", &" << valueExpr << ");\n";
                return true;
            }
            if (field.kind == FieldKind::Bool) {
                out << "        changed |= ModuGUI::Checkbox(" << labelLiteral << ", &" << valueExpr << ");\n";
                return true;
            }
            if (field.kind == FieldKind::String || field.kind == FieldKind::ObjectRef ||
                fieldLooksLikeObjectRefString(field)) {
                if (field.kind == FieldKind::ObjectRef || fieldLooksLikeObjectRefString(field)) {
                    out << "        changed |= DrawObjectRefInput(ctx, " << labelLiteral
                        << ", " << valueExpr << ");\n";
                } else {
                    out << "        changed |= ::ModuCPP::DrawStdStringInput(" << labelLiteral
                        << ", " << valueExpr << ");\n";
                }
                return true;
            }
            if (field.kind == FieldKind::Vec3) {
                out << "        changed |= ModuGUI::DragFloat3(" << labelLiteral << ", &" << valueExpr
                    << ".x, 0.01f, -1000.0f, 1000.0f, \"%.3f\");\n";
                return true;
            }
            if (fieldLooksLikeStringArray(field) || field.kind == FieldKind::ObjectList) {
                out << "        changed |= " << supportNs << "::DrawObjectRefListEditor(ctx, " << labelLiteral
                    << ", " << valueExpr << ");\n";
                return true;
            }
            if (fieldLooksLikeDialogueLineArray(field) || field.kind == FieldKind::DialogueLines) {
                out << "        changed |= " << supportNs << "::DrawDialogueLinesEditor(ctx, " << valueExpr << ");\n";
                return true;
            }
            if (findSubScriptByType(field.baseType) != nullptr) {
                if (field.arrayDimensions.empty()) {
                    out << "        changed |= ::ModuCPP::EditSubScript(" << labelLiteral << ", " << valueExpr << ");\n";
                    return true;
                }
                if (field.arrayDimensions.size() == 1 && trimCopy(field.arrayDimensions[0]).empty()) {
                    out << "        changed |= ::ModuCPP::EditSubScriptArray(" << labelLiteral << ", " << valueExpr << ");\n";
                    return true;
                }
            }
            if (field.arrayDimensions.empty()) {
                out << "        {\n";
                out << "            int enumValue = static_cast<int>(" << valueExpr << ");\n";
                out << "            if (ModuGUI::InputInt(" << labelLiteral << ", &enumValue)) {\n";
                    out << "                " << valueExpr << " = static_cast<" << cachedMapScriptTypeToCpp(field.rawType)
                    << ">(enumValue);\n";
                out << "                changed = true;\n";
                out << "            }\n";
                out << "        }\n";
                return true;
            }
            error = "Unsupported SubScript field inspector for '" + field.name + "' in " + spec.name + ".";
            return false;
        };

        bool hasInspectorMethod = false;
        for (const MethodSpec& method : spec.methods) {
            if (method.name == "Script_OnInspector") {
                hasInspectorMethod = true;
                break;
            }
        }
        const bool hasInspectorBlock = !trimCopy(spec.inspectorBlock).empty();
        if (hasInspectorMethod && hasInspectorBlock) {
            error = "ModuCPP class '" + spec.name +
                    "' cannot declare both Script_OnInspector(...) and inspector { ... }.";
            return {};
        }
        if (!customPublicFieldNames.empty() && !hasInspectorMethod && !hasInspectorBlock) {
            std::ostringstream fields;
            for (size_t i = 0; i < customPublicFieldNames.size(); ++i) {
                if (i > 0) fields << ", ";
                fields << customPublicFieldNames[i];
            }
            error = "ModuCPP class '" + spec.name +
                    "' has custom public field type(s) requiring custom inspector authoring. "
                    "Add inspector { ... } or Script_OnInspector(...). Fields: " + fields.str();
            return {};
        }

        out << "// Generated from \"" << sourcePath.lexically_normal().generic_string() << "\" by ModuCPP transpiler.\n";

        std::unordered_set<std::string> emittedIncludes;
        auto emitInclude = [&](const std::string& includeDirective) {
            const std::string normalized = trimCopy(includeDirective);
            if (normalized.empty()) return;
            if (!emittedIncludes.insert(normalized).second) return;
            out << normalized << "\n";
        };

        std::vector<std::string> rewrittenIncludes;
        rewrittenIncludes.reserve(spec.includeDirectives.size());
        bool hasExplicitScriptApiImport = false;
        for (const std::string& includeDirective : spec.includeDirectives) {
            const std::string rewritten = rewriteIncludeDirective(includeDirective, sourcePath);
            if (rewritten == "#include \"ModuCPPScriptApi.h\"" ||
                rewritten == "#include \"ModuEngineScriptApi.h\"" ||
                rewritten == "#include \"ModuInputScriptApi.h\"" ||
                rewritten == "#include \"RMeshBuilderScriptApi.h\"" ||
                rewritten == "#include \"ModuCPPExperimentalScriptApi.h\"") {
                hasExplicitScriptApiImport = true;
            }
            rewrittenIncludes.push_back(rewritten);
        }
        if (!hasExplicitScriptApiImport) {
            emitInclude("#include \"ModuCPPScriptApi.h\"");
        }
        for (const std::string& includeDirective : rewrittenIncludes) {
            emitInclude(includeDirective);
        }
        if (!spec.subScripts.empty()) {
            emitInclude("#include \"ModuCPPExperimentalScriptApi.h\"");
        }
        emitInclude("#include <algorithm>");
        emitInclude("#include <array>");
        emitInclude("#include <cassert>");
        emitInclude("#include <cctype>");
        emitInclude("#include <cstddef>");
        emitInclude("#include <cstdlib>");
        emitInclude("#include <cstdio>");
        emitInclude("#include <string>");
        emitInclude("#include <unordered_map>");
        emitInclude("#include <vector>");
        out << "\n";
        out << "using namespace ::ModuCPP;\n\n";

        std::unordered_set<std::string> emittedReachableSubScriptNames;
        for (const SubScriptSpec& subScript : spec.subScripts) {
            const std::string normalizedSubScript =
                toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(subScript.name)));
            if (reachableSubScriptTypes.find(normalizedSubScript) != reachableSubScriptTypes.end()) {
                emittedReachableSubScriptNames.insert(subScript.name);
            }
        }

        const std::string normalizedPassthrough =
            cachedRewriteSurfaceSyntax(stripIncludeDirectives(spec.passthroughCode));
        const std::string passthroughSansStructs =
            stripStructDefinitionsByName(normalizedPassthrough, emittedReachableSubScriptNames);
        const ExtractedCodeBlocks enumBlocks = extractEnumClassDefinitions(passthroughSansStructs);
        const std::string passthroughEnums = trimCopy(enumBlocks.extracted);
        const std::string passthrough = enumBlocks.remaining;
        if (!passthroughEnums.empty()) {
            out << passthroughEnums << "\n\n";
        }
        for (const SubScriptSpec& subScript : spec.subScripts) {
            const std::string normalizedSubScript =
                toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(subScript.name)));
            if (reachableSubScriptTypes.find(normalizedSubScript) == reachableSubScriptTypes.end()) {
                continue;
            }

            out << "struct " << mapScriptBaseTypeToCpp(subScript.name) << " {\n";
            for (const FieldSpec& field : subScript.fields) {
                if (field.kind == FieldKind::ObjectRef) {
                    out << "    std::string " << field.name;
                    if (!field.initializer.empty()) {
                        out << " = " << cachedRewriteSurfaceSyntax(field.initializer);
                    }
                    out << ";\n";
                } else if (field.kind == FieldKind::ObjectList) {
                    out << "    std::vector<std::string> " << field.name;
                    if (!field.initializer.empty()) {
                        out << " = " << supportNs << "::DeserializeObjectRefs("
                            << cachedRewriteSurfaceSyntax(field.initializer) << ")";
                    }
                    out << ";\n";
                } else if (field.kind == FieldKind::DialogueLines) {
                    out << "    std::vector<DialoguePort::DialogueLine> " << field.name;
                    if (!field.initializer.empty()) {
                        out << " = DialoguePort::DeserializeDialogueLines("
                            << cachedRewriteSurfaceSyntax(field.initializer) << ")";
                    }
                    out << ";\n";
                } else {
                    out << "    " << cachedMapScriptTypeToCpp(field.rawType) << " " << field.name;
                    if (!field.initializer.empty()) {
                        out << " = " << cachedRewriteSurfaceSyntax(field.initializer);
                    }
                    out << ";\n";
                }
            }
            out << "};\n\n";
        }
        if (!trimCopy(passthrough).empty()) {
            out << passthrough << "\n";
        }
        if (needsDialoguePortSupport && spec.passthroughCode.find("DialoguePortShared.h") == std::string::npos) {
            out << "#include \"" << findNearestSiblingHeader(sourcePath, "DialoguePortShared.h").string() << "\"\n";
        }

        out << "namespace " << supportNs << " {\n\n";
        out << "using namespace ::ModuCPP;\n\n";
        if (needsDialoguePortSupport) {
            out << "using namespace DialoguePort;\n\n";
        }
        if (needsObjectRefSupport) {
            out << "using ::ModuCPP::DrawObjectRefInput;\n\n";
        }
        out << "struct " << configType << " {\n";
        for (const FieldSpec& field : spec.fields) {
            if (!fieldPersists(field)) continue;
            if (field.kind == FieldKind::ObjectRef) {
                out << "    std::string " << field.name;
                if (!field.initializer.empty()) {
                    out << " = " << cachedRewriteSurfaceSyntax(field.initializer);
                }
                out << ";\n";
            } else if (field.kind == FieldKind::ObjectList) {
                out << "    std::vector<std::string> " << field.name;
                if (!field.initializer.empty()) {
                    out << " = " << supportNs << "::DeserializeObjectRefs("
                        << cachedRewriteSurfaceSyntax(field.initializer) << ")";
                }
                out << ";\n";
            } else if (field.kind == FieldKind::DialogueLines) {
                out << "    std::vector<DialoguePort::DialogueLine> " << field.name;
                if (!field.initializer.empty()) {
                    out << " = DialoguePort::DeserializeDialogueLines("
                        << cachedRewriteSurfaceSyntax(field.initializer) << ")";
                }
                out << ";\n";
            } else {
                out << "    " << cachedMapScriptTypeToCpp(field.rawType) << " " << field.name;
                if (!field.initializer.empty()) {
                    out << " = " << cachedRewriteSurfaceSyntax(field.initializer);
                }
                out << ";\n";
            }
        }
        out << "};\n\n";

        if (hasTransientFields) {
            out << "struct " << stateType << " {\n";
            for (const FieldSpec& field : spec.fields) {
                if (fieldPersists(field)) continue;
                out << "    " << cachedMapScriptTypeToCpp(field.rawType) << " " << field.name;
                if (!field.initializer.empty()) {
                    out << " = " << cachedRewriteSurfaceSyntax(field.initializer);
                }
                out << ";\n";
            }
            out << "};\n\n";
        }

        if (needsEscapedStringHelpers) {
            out << "inline std::string Trim(const std::string& value) {\n";
            out << "    size_t start = 0;\n";
            out << "    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {\n";
            out << "        ++start;\n";
            out << "    }\n";
            out << "    size_t end = value.size();\n";
            out << "    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {\n";
            out << "        --end;\n";
            out << "    }\n";
            out << "    return value.substr(start, end - start);\n";
            out << "}\n\n";

            out << "inline std::vector<std::string> SplitEscaped(const std::string& value, char delimiter) {\n";
            out << "    std::vector<std::string> fields;\n";
            out << "    std::string current;\n";
            out << "    bool escaped = false;\n";
            out << "    for (char c : value) {\n";
            out << "        if (escaped) {\n";
            out << "            if (c == 'n') current.push_back('\\n');\n";
            out << "            else if (c == 'r') current.push_back('\\r');\n";
            out << "            else current.push_back(c);\n";
            out << "            escaped = false;\n";
            out << "            continue;\n";
            out << "        }\n";
            out << "        if (c == '\\\\') {\n";
            out << "            escaped = true;\n";
            out << "            continue;\n";
            out << "        }\n";
            out << "        if (c == delimiter) {\n";
            out << "            fields.push_back(current);\n";
            out << "            current.clear();\n";
            out << "            continue;\n";
            out << "        }\n";
            out << "        current.push_back(c);\n";
            out << "    }\n";
            out << "    if (escaped) current.push_back('\\\\');\n";
            out << "    fields.push_back(current);\n";
            out << "    return fields;\n";
            out << "}\n\n";

            out << "inline std::string EscapeField(const std::string& value, char delimiter) {\n";
            out << "    std::string outValue;\n";
            out << "    outValue.reserve(value.size() + 8);\n";
            out << "    for (char c : value) {\n";
            out << "        if (c == '\\\\' || c == delimiter || c == '\\n' || c == '\\r') {\n";
            out << "            outValue.push_back('\\\\');\n";
            out << "            if (c == '\\n') outValue.push_back('n');\n";
            out << "            else if (c == '\\r') outValue.push_back('r');\n";
            out << "            else outValue.push_back(c);\n";
            out << "        } else {\n";
            out << "            outValue.push_back(c);\n";
            out << "        }\n";
            out << "    }\n";
            out << "    return outValue;\n";
            out << "}\n\n";

            out << "inline std::string JoinEscaped(const std::vector<std::string>& values, char delimiter) {\n";
            out << "    std::string joined;\n";
            out << "    for (size_t i = 0; i < values.size(); ++i) {\n";
            out << "        joined += EscapeField(values[i], delimiter);\n";
            out << "        if (i + 1 < values.size()) joined.push_back(delimiter);\n";
            out << "    }\n";
            out << "    return joined;\n";
            out << "}\n\n";
        }

        if (needsObjectListSupport) {
            out << "inline bool IsAllDigits(const std::string& value) {\n";
            out << "    if (value.empty()) return false;\n";
            out << "    size_t start = (value[0] == '-' || value[0] == '+') ? 1u : 0u;\n";
            out << "    if (start >= value.size()) return false;\n";
            out << "    for (size_t i = start; i < value.size(); ++i) {\n";
            out << "        if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;\n";
            out << "    }\n";
            out << "    return true;\n";
            out << "}\n\n";

            out << "inline std::vector<std::string> DeserializeObjectRefs(const std::string& encoded) {\n";
            out << "    std::vector<std::string> refs;\n";
            out << "    if (encoded.empty()) return refs;\n";
            out << "    const std::vector<std::string> parsed = SplitEscaped(encoded, ';');\n";
            out << "    refs.reserve(parsed.size());\n";
            out << "    for (const std::string& item : parsed) {\n";
            out << "        std::string trimmed = Trim(item);\n";
            out << "        if (!trimmed.empty()) refs.push_back(trimmed);\n";
            out << "    }\n";
            out << "    return refs;\n";
            out << "}\n\n";

            out << "inline std::string SerializeObjectRefs(const std::vector<std::string>& refs) {\n";
            out << "    std::vector<std::string> cleaned;\n";
            out << "    cleaned.reserve(refs.size());\n";
            out << "    for (const std::string& ref : refs) {\n";
            out << "        std::string trimmed = Trim(ref);\n";
            out << "        if (!trimmed.empty()) cleaned.push_back(trimmed);\n";
            out << "    }\n";
            out << "    return JoinEscaped(cleaned, ';');\n";
            out << "}\n\n";

            out << "inline SceneObject* ResolveSceneObjectRef(ScriptContext& ctx, const std::string& objectRef) {\n";
            out << "    std::string trimmed = Trim(objectRef);\n";
            out << "    if (trimmed.empty()) return nullptr;\n";
            out << "    if (SceneObject* resolved = ctx.ResolveObjectRef(trimmed)) {\n";
            out << "        return resolved;\n";
            out << "    }\n";
            out << "    if (IsAllDigits(trimmed)) {\n";
            out << "        return ctx.FindObjectById(std::atoi(trimmed.c_str()));\n";
            out << "    }\n";
            out << "    return ctx.FindObjectByName(trimmed);\n";
            out << "}\n\n";

            out << "inline std::vector<SceneObject*> ResolveObjectList(ScriptContext& ctx, const std::string& encodedRefs) {\n";
            out << "    std::vector<SceneObject*> resolved;\n";
            out << "    const std::vector<std::string> refs = DeserializeObjectRefs(encodedRefs);\n";
            out << "    resolved.reserve(refs.size());\n";
            out << "    for (const std::string& ref : refs) {\n";
            out << "        resolved.push_back(ResolveSceneObjectRef(ctx, ref));\n";
            out << "    }\n";
            out << "    return resolved;\n";
            out << "}\n\n";

            out << "inline std::vector<SceneObject*> ResolveObjectList(ScriptContext& ctx,\n";
            out << "                                              const std::vector<std::string>& refs) {\n";
            out << "    std::vector<SceneObject*> resolved;\n";
            out << "    resolved.reserve(refs.size());\n";
            out << "    for (const std::string& ref : refs) {\n";
            out << "        resolved.push_back(ResolveSceneObjectRef(ctx, ref));\n";
            out << "    }\n";
            out << "    return resolved;\n";
            out << "}\n\n";
        }

        if (needsDialogueLinesSupport) {
            out << "inline bool DrawDialogueLinesEditor(ScriptContext& ctx,\n";
            out << "                                      std::vector<DialoguePort::DialogueLine>& lines) {\n";
            out << "    static std::unordered_map<const void*, int> selectedByLines;\n";
            out << "    int& selectedIndex = selectedByLines[static_cast<const void*>(&lines)];\n";
            out << "    return DialoguePort::DrawDialogueLineEditor(ctx, lines, selectedIndex);\n";
            out << "}\n\n";
        }

        if (needsObjectListSupport) {
            out << "inline void SetResolvedObjectEnabled(ScriptContext& ctx, SceneObject* obj, bool enabled) {\n";
            out << "    if (!obj) return;\n";
            out << "    bool changed = false;\n";
            out << "    if (obj->enabled != enabled) {\n";
            out << "        obj->enabled = enabled;\n";
            out << "        changed = true;\n";
            out << "    }\n";
            out << "    if (obj->hasCollider && obj->collider.enabled != enabled) {\n";
            out << "        obj->collider.enabled = enabled;\n";
            out << "        changed = true;\n";
            out << "    }\n";
            out << "    if (obj->hasCollider2D && obj->collider2D.enabled != enabled) {\n";
            out << "        obj->collider2D.enabled = enabled;\n";
            out << "        changed = true;\n";
            out << "    }\n";
            out << "    if (changed) {\n";
            out << "        ctx.MarkDirty();\n";
            out << "    }\n";
            out << "}\n\n";

            out << "inline bool DrawObjectRefListEditor(ScriptContext& ctx, const char* label,\n";
            out << "                                   std::vector<std::string>& refs) {\n";
            out << "    bool changed = false;\n";
            out << "    if (!ModuGUI::TreeNode(label)) {\n";
            out << "        return false;\n";
            out << "    }\n";
            out << "    for (size_t i = 0; i < refs.size(); ++i) {\n";
            out << "        ModuGUI::PushID(static_cast<int>(i));\n";
            out << "        const std::string rowLabel = std::string(\"Reference \") + std::to_string(i + 1);\n";
            out << "        changed |= DrawObjectRefInput(ctx, rowLabel.c_str(), refs[i]);\n";
            out << "        if (ModuGUI::SmallButton(\"Remove\")) {\n";
            out << "            refs.erase(refs.begin() + static_cast<std::ptrdiff_t>(i));\n";
            out << "            changed = true;\n";
            out << "            ModuGUI::PopID();\n";
            out << "            --i;\n";
            out << "            continue;\n";
            out << "        }\n";
            out << "        ModuGUI::PopID();\n";
            out << "    }\n";
            out << "    if (ModuGUI::Button(\"Add Reference\")) {\n";
            out << "        refs.emplace_back();\n";
            out << "        changed = true;\n";
            out << "    }\n";
            out << "    ModuGUI::SameLine();\n";
            out << "    if (ModuGUI::Button(\"Add Selected\")) {\n";
            out << "        int selectedId = ctx.GetSelectedObjectId();\n";
            out << "        if (selectedId >= 0) {\n";
            out << "            refs.push_back(std::string(\"Object.ID-\") + std::to_string(selectedId));\n";
            out << "            changed = true;\n";
            out << "        }\n";
            out << "    }\n";
            out << "    if (ModuGUI::BeginDragDropTarget()) {\n";
            out << "        if (const ImGuiPayload* payload = ModuGUI::AcceptDragDropPayload(\"SCENE_OBJECT\")) {\n";
            out << "            if (payload->Data && payload->DataSize == static_cast<int>(sizeof(int))) {\n";
            out << "                int droppedId = *static_cast<const int*>(payload->Data);\n";
            out << "                refs.push_back(std::string(\"Object.ID-\") + std::to_string(droppedId));\n";
            out << "                changed = true;\n";
            out << "            }\n";
            out << "        }\n";
            out << "        ModuGUI::EndDragDropTarget();\n";
            out << "    }\n";
            out << "    ModuGUI::TreePop();\n";
            out << "    return changed;\n";
            out << "}\n\n";
        }

        // SubScriptSerializer specializations must be defined in ::ModuCPP::detail at
        // global (non-nested) scope.  If we are currently inside the transpiled
        // namespace we have to close it first, then reopen it afterwards for the
        // remaining BindConfig / helper code.
        if (hasReachableSubScripts) {
            out << "} // namespace " << supportNs << "\n\n";
        }

        for (const SubScriptSpec& subScript : spec.subScripts) {
            const std::string normalizedSubScript =
                toLowerCopy(removeWhitespaceCopy(mapScriptBaseTypeToCpp(subScript.name)));
            if (reachableSubScriptTypes.find(normalizedSubScript) == reachableSubScriptTypes.end()) {
                continue;
            }
            out << "namespace ModuCPP::detail {\n";
            // Bring in the transpiled-namespace helpers (SerializeObjectRefs,
            // DeserializeObjectRefs, Trim, …) so the specialization body can use
            // them without full qualification.
            out << "using namespace " << supportNs << ";\n";
            out << "template <> struct SubScriptSerializer<" << mapScriptBaseTypeToCpp(subScript.name) << "> {\n";
            out << "    static std::string Serialize(const " << mapScriptBaseTypeToCpp(subScript.name) << "& value) {\n";
            out << "        std::vector<std::string> fields;\n";
            out << "        fields.reserve(" << subScript.fields.size() << ");\n";
            for (const FieldSpec& field : subScript.fields) {
                const std::optional<std::string> expr = subScriptFieldSerializeExpr(field, "value." + field.name);
                if (!expr.has_value()) {
                    error = "Unsupported SubScript field serialization for '" + field.name + "' in " + subScript.name + ".";
                    return {};
                }
                out << "        fields.push_back(" << *expr << ");\n";
            }
            out << "        return " << supportNs << "::JoinEscaped(fields, '|');\n";
            out << "    }\n";
            out << "    static " << mapScriptBaseTypeToCpp(subScript.name) << " Deserialize(const std::string& encoded) {\n";
            out << "        " << mapScriptBaseTypeToCpp(subScript.name) << " value{};\n";
            out << "        if (encoded.empty()) return value;\n";
            out << "        const std::vector<std::string> fields = " << supportNs << "::SplitEscaped(encoded, '|');\n";
            for (size_t fieldIndex = 0; fieldIndex < subScript.fields.size(); ++fieldIndex) {
                if (!emitSubScriptFieldDeserialize(fieldIndex, subScript.fields[fieldIndex],
                                                "fields[" + std::to_string(fieldIndex) + "]",
                                                "value." + subScript.fields[fieldIndex].name)) {
                    return {};
                }
            }
            out << "        return value;\n";
            out << "    }\n";
            out << "    static std::string SerializeArray(const std::vector<" << mapScriptBaseTypeToCpp(subScript.name)
                << ">& values) {\n";
            out << "        std::vector<std::string> encoded;\n";
            out << "        encoded.reserve(values.size());\n";
            out << "        for (const auto& item : values) {\n";
            out << "            encoded.push_back(Serialize(item));\n";
            out << "        }\n";
            out << "        return " << supportNs << "::JoinEscaped(encoded, '\\t');\n";
            out << "    }\n";
            out << "    static std::vector<" << mapScriptBaseTypeToCpp(subScript.name)
                << "> DeserializeArray(const std::string& encoded) {\n";
            out << "        std::vector<" << mapScriptBaseTypeToCpp(subScript.name) << "> values;\n";
            out << "        if (encoded.empty()) return values;\n";
            out << "        char delimiter = '\\t';\n";
            out << "        if (encoded.find('\\t') == std::string::npos && encoded.find('\\n') != std::string::npos) {\n";
            out << "            delimiter = '\\n';\n";
            out << "        }\n";
            out << "        const std::vector<std::string> entries = " << supportNs << "::SplitEscaped(encoded, delimiter);\n";
            out << "        values.reserve(entries.size());\n";
            out << "        for (const std::string& entry : entries) {\n";
            out << "            if (" << supportNs << "::Trim(entry).empty()) continue;\n";
            out << "            values.push_back(Deserialize(entry));\n";
            out << "        }\n";
            out << "        return values;\n";
            out << "    }\n";
            out << "    static bool Edit(const char* label, " << mapScriptBaseTypeToCpp(subScript.name) << "& value) {\n";
            out << "        bool changed = false;\n";
            out << "        const bool hasLabel = label && *label;\n";
            out << "        ScriptContext* scriptCtx = ::ModuCPP::ctxPtr();\n";
            const bool subScriptNeedsContext = std::any_of(subScript.fields.begin(), subScript.fields.end(),
                                                        [&](const FieldSpec& field) {
                                                            return field.kind == FieldKind::ObjectRef ||
                                                                    field.kind == FieldKind::ObjectList ||
                                                                    field.kind == FieldKind::DialogueLines ||
                                                                    fieldLooksLikeStringArray(field) ||
                                                                    fieldLooksLikeDialogueLineArray(field) ||
                                                                    fieldLooksLikeObjectRefString(field);
                                                        });
            if (subScriptNeedsContext) {
                out << "        if (!scriptCtx) return false;\n";
                out << "        ScriptContext& ctx = *scriptCtx;\n";
            }
            out << "        if (hasLabel && !ModuGUI::TreeNode(label)) {\n";
            out << "            return false;\n";
            out << "        }\n";
            for (const FieldSpec& field : subScript.fields) {
                if (!emitSubScriptFieldEdit(field,
                                            "\"" + escapeCStringLiteral(inspectorLabelFromFieldName(field.name)) + "\"",
                                            "value." + field.name)) {
                    return {};
                }
            }
            out << "        if (hasLabel) ModuGUI::TreePop();\n";
            out << "        return changed;\n";
            out << "    }\n";
            out << "    static bool EditArray(const char* label, std::vector<" << mapScriptBaseTypeToCpp(subScript.name)
                << ">& values) {\n";
            out << "        bool changed = false;\n";
            out << "        if (!ModuGUI::TreeNode(label)) {\n";
            out << "            return false;\n";
            out << "        }\n";
            out << "        for (size_t i = 0; i < values.size(); ++i) {\n";
            out << "            ModuGUI::PushID(static_cast<int>(i));\n";
            out << "            const std::string itemLabel = std::string(\"Item \") + std::to_string(i + 1);\n";
            out << "            changed |= Edit(itemLabel.c_str(), values[i]);\n";
            out << "            if (ModuGUI::SmallButton(\"Remove\")) {\n";
            out << "                values.erase(values.begin() + static_cast<std::ptrdiff_t>(i));\n";
            out << "                changed = true;\n";
            out << "                ModuGUI::PopID();\n";
            out << "                --i;\n";
            out << "                continue;\n";
            out << "            }\n";
            out << "            ModuGUI::Separator();\n";
            out << "            ModuGUI::PopID();\n";
            out << "        }\n";
            out << "        if (ModuGUI::Button(\"Add Item\")) {\n";
            out << "            values.emplace_back();\n";
            out << "            changed = true;\n";
            out << "        }\n";
            out << "        ModuGUI::TreePop();\n";
            out << "        return changed;\n";
            out << "    }\n";
            out << "};\n";
            out << "} // namespace ModuCPP::detail\n\n";
        }

        if (hasReachableSubScripts) {
            // Reopen the transpiled namespace for BindConfig and the rest of the
            // generated code that follows.
            out << "namespace " << supportNs << " {\n\n";
            out << "using namespace ::ModuCPP;\n";
            if (needsDialoguePortSupport) {
                out << "using namespace DialoguePort;\n";
            }
            out << "\n";
        }

        out << "inline void BindConfig(ScriptContext& ctx, " << configType << "& config) {\n";
        bool hasPersistedFields = false;
        for (const FieldSpec& field : spec.fields) {
            if (!fieldPersists(field)) continue;
            hasPersistedFields = true;
            if (field.kind == FieldKind::Custom) {
                if (fieldUsesSupportedSubScript(field)) {
                    if (field.arrayDimensions.empty()) {
                        out << "    config." << field.name << " = ctx.GetSetting(\"" << field.name
                            << "\", \"\").empty() ? config." << field.name
                            << " : ::ModuCPP::DeserializeSubScript<" << mapScriptBaseTypeToCpp(field.baseType)
                            << ">(ctx.GetSetting(\"" << field.name << "\", \"\"));\n";
                    } else if (field.arrayDimensions.size() == 1 && trimCopy(field.arrayDimensions[0]).empty()) {
                        out << "    config." << field.name << " = ctx.GetSetting(\"" << field.name
                            << "\", \"\").empty() ? config." << field.name
                            << " : ::ModuCPP::DeserializeSubScriptArray<" << mapScriptBaseTypeToCpp(field.baseType)
                            << ">(ctx.GetSetting(\"" << field.name << "\", \"\"));\n";
                    }
                } else if (fieldLooksLikeDialogueLineArray(field)) {
                    out << "    config." << field.name << " = DialoguePort::DeserializeDialogueLines(ctx.GetSetting(\""
                        << field.name << "\", DialoguePort::SerializeDialogueLines(config." << field.name
                        << ")));\n";
                } else if (field.arrayDimensions.empty()) {
                    out << "    if (!ctx.GetSetting(\"" << field.name << "\", \"\").empty()) {\n";
                    out << "        config." << field.name << " = static_cast<" << cachedMapScriptTypeToCpp(field.rawType)
                        << ">(std::atoi(ctx.GetSetting(\"" << field.name << "\", \"\").c_str()));\n";
                    out << "    }\n";
                } else if (field.arrayDimensions.size() == 1 && trimCopy(field.arrayDimensions[0]).empty()) {
                    out << "    (void)ctx;\n";
                } else if (field.arrayDimensions.size() == 1) {
                    out << "    ::ModuCPP::BindArray(ctx, \"" << field.name << "\", config." << field.name << ");\n";
                } else if (field.arrayDimensions.size() == 2) {
                    out << "    ::ModuCPP::BindArray2D(ctx, \"" << field.name << "\", config." << field.name << ");\n";
                }
                continue;
            }
            if (field.kind == FieldKind::ObjectList) {
                out << "    config." << field.name << " = DeserializeObjectRefs(ctx.GetSetting(\""
                    << field.name << "\", SerializeObjectRefs(config." << field.name << ")));\n";
            } else if (field.kind == FieldKind::ObjectRef) {
                out << "    config." << field.name << " = ctx.GetSetting(\""
                    << field.name << "\", config." << field.name << ");\n";
            } else if (field.kind == FieldKind::DialogueLines) {
                out << "    config." << field.name << " = DialoguePort::DeserializeDialogueLines(ctx.GetSetting(\""
                    << field.name << "\", DialoguePort::SerializeDialogueLines(config." << field.name
                    << ")));\n";
            } else {
                out << "    ::ModuCPP::BindSetting(ctx, \"" << field.name << "\", config." << field.name << ");\n";
            }
        }
        if (!hasPersistedFields) {
            out << "    (void)ctx;\n";
            out << "    (void)config;\n";
        }
        out << "}\n\n";
        out << "} // namespace " << supportNs << "\n\n";

        auto findPersistedFieldByName = [&](const std::string& fieldName) -> const FieldSpec* {
            for (const FieldSpec& field : spec.fields) {
                if (fieldPersists(field) && field.name == fieldName) {
                    return &field;
                }
            }
            return nullptr;
        };

        for (const MethodSpec& method : spec.methods) {
            if (toLowerCopy(removeWhitespaceCopy(method.returnType)) == "auto") {
                continue;
            }
            if (method.isStatic) out << "static ";
            out << method.returnType << " " << method.name << "(" << method.transpiledParams << ");\n";
        }
        if (!spec.methods.empty()) {
            out << "\n";
        }

            if (hasInspectorBlock) {
            out << "extern \"C\" MODULARITY_SCRIPT_EXPORT void Script_OnInspector(ScriptContext& ctx) {\n";
            out << "    MODU_SCRIPT(ctx);\n";
            if (!spec.fields.empty()) {
                out << "    auto& config = ::ModuCPP::Config<" << supportNs << "::" << configType << ">();\n";
                out << "    " << supportNs << "::BindConfig(ctx, config);\n";
                for (const FieldSpec& field : spec.fields) {
                    if (!fieldPersists(field)) continue;
                    out << "    auto& " << field.name << " = config." << field.name << ";\n";
                }
                if (hasTransientFields) {
                    out << "    auto& state = ::ModuCPP::State<" << supportNs << "::" << stateType << ">();\n";
                    for (const FieldSpec& field : spec.fields) {
                        if (fieldPersists(field)) continue;
                        out << "    auto& " << field.name << " = state." << field.name << ";\n";
                    }
                }
            }
            std::vector<const FieldSpec*> inspectorSnapshotFields;
            for (const FieldSpec& field : spec.fields) {
                if (!fieldPersists(field)) continue;
                if (field.kind == FieldKind::DialogueLines || fieldLooksLikeDialogueLineArray(field)) {
                    inspectorSnapshotFields.push_back(&field);
                }
            }
            for (const FieldSpec* field : inspectorSnapshotFields) {
                out << "    const std::string _moduBefore_" << field->name
                    << " = DialoguePort::SerializeDialogueLines(config." << field->name << ");\n";
            }
            out << "    bool changed = false;\n";

            int inspectorTempCounter = 0;
            bool hasExplicitInspectorSave = false;
            std::unordered_set<std::string> autoFieldsSkipped;
            auto nextInspectorTemp = [&](const std::string& prefix) {
                ++inspectorTempCounter;
                return "_modu" + prefix + std::to_string(inspectorTempCounter);
            };

            std::function<bool(const std::string&, const std::vector<std::string>&, const std::string&)> emitInspectorCall;
            emitInspectorCall = [&](const std::string& callName, const std::vector<std::string>& args,
                                    const std::string& indent) -> bool {
                auto requireArgs = [&](size_t count, const char* usage) -> bool {
                    if (args.size() != count) {
                        error = std::string("inspector ") + usage;
                        return false;
                    }
                    return true;
                };

                auto emitAutoLabel = [&](const std::string& expr) {
                    return escapeCStringLiteral(inspectorLabelFromExpression(expr));
                };
                auto parseLabelValue = [&](const std::vector<std::string>& widgetArgs,
                                        const std::string& usage,
                                        std::string& outLabelLiteral,
                                        std::string& outExpr) -> bool {
                    if (widgetArgs.size() == 1) {
                        outExpr = trimCopy(widgetArgs[0]);
                        outLabelLiteral = "\"" + emitAutoLabel(outExpr) + "\"";
                        return true;
                    }
                    if (widgetArgs.size() == 2) {
                        outLabelLiteral = trimCopy(widgetArgs[0]);
                        outExpr = trimCopy(widgetArgs[1]);
                        return true;
                    }
                    error = "inspector " + usage;
                    return false;
                };

                // Later TODO: Make this a switch state instead of a if statement staircase lmfao.
                if (callName == "Config") {
                    if (!requireArgs(2, "Config(Type, varName) requires exactly two arguments.")) return false;
                    const std::string typeExpr = trimCopy(args[0]);
                    const std::string varExpr = trimCopy(args[1]);
                    if (typeExpr.empty() || varExpr.empty()) {
                        error = "inspector Config(Type, varName) requires non-empty type and variable.";
                        return false;
                    }
                    out << indent << typeExpr << " " << varExpr << " = loadConfig(ctx);\n";
                    return true;
                }
                if (callName == "AutoSave" || callName == "Save") {
                    if (!requireArgs(1, "AutoSave(configVar) requires exactly one argument.")) return false;
                    const std::string expr = trimCopy(args[0]);
                    if (expr.empty()) {
                        error = "inspector AutoSave(configVar) requires non-empty argument.";
                        return false;
                    }
                    out << indent << "if (changed) {\n";
                    out << indent << "    saveConfig(ctx, " << expr << ");\n";
                    out << indent << "}\n";
                    hasExplicitInspectorSave = true;
                    return true;
                }

                if (callName == "Header") {
                    if (!requireArgs(1, "Header(title) requires exactly one argument.")) return false;
                    out << indent << "ModuGUI::TextUnformatted(" << trimCopy(args[0]) << ");\n";
                    out << indent << "ModuGUI::Separator();\n";
                    return true;
                }
                if (callName == "Run") {
                    if (!requireArgs(1, "Run(expression) requires exactly one argument.")) return false;
                    out << indent << trimCopy(args[0]) << ";\n";
                    return true;
                }
                if (callName == "Separator") {
                    if (!args.empty()) {
                        error = "inspector Separator() does not take arguments.";
                        return false;
                    }
                    out << indent << "ModuGUI::Separator();\n";
                    return true;
                }
                if (callName == "Toggle") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "Toggle(value) or Toggle(label, value) expected.",
                                        labelExpr, expr)) return false;
                    out << indent << "changed |= ModuGUI::Checkbox(" << labelExpr
                        << ", &" << expr << ");\n";
                    return true;
                }

                // Future me: nevermind, it works fine now, it wasn't even caused by ModuCPP lmfao.
                if (callName == "Slider") {
                    std::string labelExpr;
                    std::string expr;
                    std::string minExpr;
                    std::string maxExpr;
                    if (args.size() == 3) {
                        expr = trimCopy(args[0]);
                        minExpr = trimCopy(args[1]);
                        maxExpr = trimCopy(args[2]);
                        labelExpr = "\"" + emitAutoLabel(expr) + "\"";
                    } else if (args.size() == 4) {
                        labelExpr = trimCopy(args[0]);
                        expr = trimCopy(args[1]);
                        minExpr = trimCopy(args[2]);
                        maxExpr = trimCopy(args[3]);
                    } else {
                        error = "inspector Slider(value,min,max) or Slider(label,value,min,max) expected.";
                        return false;
                    }

                    const FieldSpec* field = findPersistedFieldByName(expr);
                    if (field && field->kind == FieldKind::Float) {
                        const std::string stepExpr = field->stepExpr.value_or("0.01f");
                        out << indent << "changed |= ModuGUI::DragFloat(" << labelExpr
                            << ", &" << expr << ", " << stepExpr << ", "
                            << minExpr << ", " << maxExpr << ");\n";
                        return true;
                    }
                    if (field && field->kind == FieldKind::Int) {
                        out << indent << "changed |= ModuGUI::SliderInt(" << labelExpr
                            << ", &" << expr << ", static_cast<int>(" << minExpr
                            << "), static_cast<int>(" << maxExpr << "));\n";
                        return true;
                    }
                    out << indent << "{\n";
                    out << indent << "    auto& _moduSliderValue = " << expr << ";\n";
                    out << indent << "    using _ModuSliderType = std::remove_reference_t<decltype(_moduSliderValue)>;\n";
                    out << indent << "    if constexpr (std::is_floating_point_v<_ModuSliderType>) {\n";
                    out << indent << "        changed |= ModuGUI::DragFloat(" << labelExpr
                        << ", &_moduSliderValue, 0.01f, " << minExpr << ", " << maxExpr << ");\n";
                    out << indent << "    } else if constexpr (std::is_integral_v<_ModuSliderType>) {\n";
                    out << indent << "        int _moduSliderInt = static_cast<int>(_moduSliderValue);\n";
                    out << indent << "        if (ModuGUI::SliderInt(" << labelExpr
                        << ", &_moduSliderInt, static_cast<int>(" << minExpr
                        << "), static_cast<int>(" << maxExpr << "))) {\n";
                    out << indent << "            _moduSliderValue = static_cast<_ModuSliderType>(_moduSliderInt);\n";
                    out << indent << "            changed = true;\n";
                    out << indent << "        }\n";
                    out << indent << "    }\n";
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "Number") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "Number(value) or Number(label, value) expected.",
                                        labelExpr, expr)) return false;
                    out << indent << "{\n";
                    out << indent << "    auto& _moduNumberValue = " << expr << ";\n";
                    out << indent << "    using _ModuNumberType = std::remove_reference_t<decltype(_moduNumberValue)>;\n";
                    out << indent << "    if constexpr (std::is_floating_point_v<_ModuNumberType>) {\n";
                    out << indent << "        changed |= ModuGUI::DragFloat(" << labelExpr
                        << ", &_moduNumberValue, 0.01f);\n";
                    out << indent << "    } else if constexpr (std::is_integral_v<_ModuNumberType>) {\n";
                    out << indent << "        int _moduNumberInt = static_cast<int>(_moduNumberValue);\n";
                    out << indent << "        if (ModuGUI::InputInt(" << labelExpr
                        << ", &_moduNumberInt)) {\n";
                    out << indent << "            _moduNumberValue = static_cast<_ModuNumberType>(_moduNumberInt);\n";
                    out << indent << "            changed = true;\n";
                    out << indent << "        }\n";
                    out << indent << "    }\n";
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "String") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "String(value) or String(label, value) expected.",
                                        labelExpr, expr)) return false;
                    out << indent << "changed |= DrawStdStringInput(" << labelExpr
                        << ", " << expr << ", 512);\n";
                    return true;
                }
                if (callName == "ObjectRef") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "ObjectRef(value) or ObjectRef(label, value) expected.",
                                        labelExpr, expr)) return false;
                    out << indent << "changed |= DrawObjectRefInput(ctx, " << labelExpr
                        << ", " << expr << ");\n";
                    return true;
                }
                if (callName == "AudioClip") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "AudioClip(value) or AudioClip(label, value) expected.",
                                        labelExpr, expr)) return false;
                    out << indent << "changed |= DrawAudioClipInput(" << labelExpr
                        << ", " << expr << ", 512);\n";
                    return true;
                }
                if (callName == "ObjectList") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "ObjectList(value) or ObjectList(label, value) expected.",
                                        labelExpr, expr)) return false;
                    const FieldSpec* field = findPersistedFieldByName(identifierTailFromExpression(expr));
                    if (field && field->kind == FieldKind::ObjectList) {
                        const std::string changedVar = nextInspectorTemp("Changed");
                        out << indent << "{\n";
                        out << indent << "    const bool " << changedVar << " = " << supportNs << "::DrawObjectRefListEditor(ctx, "
                            << labelExpr << ", " << expr << ");\n";
                        out << indent << "    changed |= " << changedVar << ";\n";
                        out << indent << "    if (" << changedVar << ") {\n";
                        out << indent << "        ctx.SetSetting(\"" << field->name
                            << "\", " << supportNs << "::SerializeObjectRefs(" << expr << "));\n";
                        out << indent << "    }\n";
                        out << indent << "}\n";
                    } else {
                        out << indent << "changed |= " << supportNs << "::DrawObjectRefListEditor(ctx, " << labelExpr << ", "
                            << expr << ");\n";
                    }
                    return true;
                }
                if (callName == "Enum") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "Enum(value) or Enum(label, value) expected.",
                                        labelExpr, expr)) return false;
                    if (identifierTailFromExpression(expr) == "currentLanguage") {
                        out << indent << "changed |= DrawLanguageCombo(" << labelExpr
                            << ", " << expr << ");\n";
                        return true;
                    }
                    out << indent << "{\n";
                    const std::string tempVar = nextInspectorTemp("Enum");
                    out << indent << "    int " << tempVar << " = static_cast<int>(" << expr << ");\n";
                    out << indent << "    if (ModuGUI::InputInt(" << labelExpr
                        << ", &" << tempVar << ")) {\n";
                    out << indent << "        using _ModuEnumType = std::remove_reference_t<decltype(" << expr << ")>;\n";
                    out << indent << "        " << expr << " = static_cast<_ModuEnumType>(" << tempVar << ");\n";
                    if (const FieldSpec* field = findPersistedFieldByName(expr);
                        field && field->kind == FieldKind::Custom && field->arrayDimensions.empty()) {
                        out << indent << "        ctx.SetSetting(\"" << field->name
                            << "\", std::to_string(static_cast<int>(" << expr << ")));\n";
                        out << indent << "        ctx.MarkDirty();\n";
                    }
                    out << indent << "        changed = true;\n";
                    out << indent << "    }\n";
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "DialogueLines") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "DialogueLines(lines) or DialogueLines(label, lines) expected.",
                                        labelExpr, expr)) return false;
                    const FieldSpec* field = findPersistedFieldByName(identifierTailFromExpression(expr));
                    const std::string changedVar = nextInspectorTemp("Changed");
                    out << indent << "{\n";
                    out << indent << "    const bool " << changedVar << " = " << supportNs
                        << "::DrawDialogueLinesEditor(ctx, "
                        << expr << ");\n";
                    out << indent << "    changed |= " << changedVar << ";\n";
                    if (field && (field->kind == FieldKind::DialogueLines || fieldLooksLikeDialogueLineArray(*field))) {
                        out << indent << "    if (" << changedVar << ") {\n";
                        out << indent << "        ctx.SetSetting(\"" << field->name
                            << "\", DialoguePort::SerializeDialogueLines(" << expr << "));\n";
                        out << indent << "    }\n";
                    }
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "AutoFields") {
                    if (args.empty()) {
                        error = "inspector AutoFields(fieldA, fieldB, ...) requires at least one argument.";
                        return false;
                    }
                    for (const std::string& rawArg : args) {
                        const std::string fieldName = identifierTailFromExpression(rawArg);
                        if (fieldName.empty()) {
                            error = "inspector AutoFields expects field names.";
                            return false;
                        }
                        const FieldSpec* field = nullptr;
                        size_t fieldIndex = 0;
                        for (size_t i = 0; i < spec.fields.size(); ++i) {
                            if (spec.fields[i].name == fieldName) {
                                field = &spec.fields[i];
                                fieldIndex = i;
                                break;
                            }
                        }
                        if (!field || !fieldPersists(*field)) {
                            error = "inspector AutoFields references unknown field: " + fieldName;
                            return false;
                        }
                        if (!emitAutoInspectorField(fieldIndex, *field, autoFieldsSkipped)) {
                            return false;
                        }
                    }
                    return true;
                }
                if (callName == "InteractionOptions") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "InteractionOptions(options) or InteractionOptions(label, options) expected.",
                                        labelExpr, expr)) return false;
                    const std::string idVar = nextInspectorTemp("ObjId");
                    const std::string optVar = nextInspectorTemp("SelectedOption");
                    const std::string lineVar = nextInspectorTemp("SelectedLine");
                    out << indent << "{\n";
                    out << indent << "    ModuGUI::TextUnformatted(" << labelExpr << ");\n";
                    out << indent << "    const int " << idVar << " = ctx.object ? ctx.object->id : -1;\n";
                    out << indent << "    int& " << optVar << " = g_selectedOptionEditorIndex[" << idVar << "];\n";
                    out << indent << "    int& " << lineVar << " = g_selectedDialogueLineEditorIndex[" << idVar << "];\n";
                    out << indent << "    changed |= drawInteractionOptionsEditor(ctx, " << expr
                        << ", " << optVar << ", " << lineVar << ");\n";
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "MenuActions") {
                    std::string labelExpr;
                    std::string menuRefsExpr;
                    std::string actionsExpr;
                    if (args.size() == 2) {
                        menuRefsExpr = trimCopy(args[0]);
                        actionsExpr = trimCopy(args[1]);
                        labelExpr = "\"" + emitAutoLabel(actionsExpr) + "\"";
                    } else if (args.size() == 3) {
                        labelExpr = trimCopy(args[0]);
                        menuRefsExpr = trimCopy(args[1]);
                        actionsExpr = trimCopy(args[2]);
                    } else {
                        error = "inspector MenuActions(menuItemRefs, actions) or MenuActions(label, menuItemRefs, actions) expected.";
                        return false;
                    }
                    const std::string idVar = nextInspectorTemp("ObjId");
                    const std::string selVar = nextInspectorTemp("SelectedAction");
                    out << indent << "{\n";
                    out << indent << "    ModuGUI::TextUnformatted(" << labelExpr << ");\n";
                    out << indent << "    const int " << idVar << " = ctx.object ? ctx.object->id : -1;\n";
                    out << indent << "    int& " << selVar << " = g_selectedActionByObject[" << idVar << "];\n";
                    out << indent << "    changed |= drawMenuActionsEditor(ctx, " << menuRefsExpr
                        << ", " << actionsExpr << ", " << selVar << ");\n";
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "RuntimeDialogueStatus") {
                    if (!args.empty()) {
                        error = "inspector RuntimeDialogueStatus() does not take arguments.";
                        return false;
                    }
                    const std::string idVar = nextInspectorTemp("ObjId");
                    const std::string itVar = nextInspectorTemp("StateIt");
                    out << indent << "{\n";
                    out << indent << "    const int " << idVar << " = ctx.object ? ctx.object->id : -1;\n";
                    out << indent << "    auto " << itVar << " = g_runtimeStates.find(" << idVar << ");\n";
                    out << indent << "    DialoguePort::DrawDialogueRuntimeStatus(" << itVar << " != g_runtimeStates.end() ? &"
                        << itVar << "->second : nullptr);\n";
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "RuntimeInteractableStatus") {
                    if (!args.empty()) {
                        error = "inspector RuntimeInteractableStatus() does not take arguments.";
                        return false;
                    }
                    const std::string idVar = nextInspectorTemp("ObjId");
                    const std::string itVar = nextInspectorTemp("StateIt");
                    out << indent << "{\n";
                    out << indent << "    const int " << idVar << " = ctx.object ? ctx.object->id : -1;\n";
                    out << indent << "    auto " << itVar << " = g_runtimeStates.find(" << idVar << ");\n";
                    out << indent << "    drawRuntimeStatus(config, " << itVar << " != g_runtimeStates.end() ? &"
                        << itVar << "->second : nullptr);\n";
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "RuntimeMenuStatus") {
                    if (!args.empty()) {
                        error = "inspector RuntimeMenuStatus() does not take arguments.";
                        return false;
                    }
                    const std::string idVar = nextInspectorTemp("ObjId");
                    const std::string itVar = nextInspectorTemp("StateIt");
                    out << indent << "{\n";
                    out << indent << "    const int " << idVar << " = ctx.object ? ctx.object->id : -1;\n";
                    out << indent << "    auto " << itVar << " = g_runtimeStates.find(" << idVar << ");\n";
                    out << indent << "    drawRuntimeStatus(config, " << itVar << " != g_runtimeStates.end() ? &"
                        << itVar << "->second : nullptr);\n";
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "TextEffectFlags") {
                    std::string labelExpr;
                    std::string expr;
                    if (!parseLabelValue(args, "TextEffectFlags(effect) or TextEffectFlags(label, effect) expected.",
                                        labelExpr, expr)) return false;
                    const std::string beforeVar = nextInspectorTemp("BeforeEffect");
                    out << indent << "{\n";
                    out << indent << "    ModuGUI::TextUnformatted(" << labelExpr << ");\n";
                    out << indent << "    auto " << beforeVar << " = " << expr << ";\n";
                    out << indent << "    drawTextEffectEditor(" << expr << ");\n";
                    out << indent << "    changed |= (" << expr << " != " << beforeVar << ");\n";
                    out << indent << "}\n";
                    return true;
                }
                if (callName == "ClipGrid") {
                    if (!requireArgs(2, "ClipGrid(idleClips, walkClips) requires exactly two arguments.")) return false;
                    out << indent << "changed |= EditDirectionalClipGrid(" << trimCopy(args[0])
                        << ", " << trimCopy(args[1]) << ");\n";
                    return true;
                }
                if (callName == "SoundSet") {
                    if (!requireArgs(2, "SoundSet(label, sounds) requires exactly two arguments.")) return false;
                    out << indent << "changed |= EditSoundSet(" << trimCopy(args[0])
                        << ", " << trimCopy(args[1]) << ");\n";
                    return true;
                }

                error = "Unsupported inspector statement: " + callName;
                return false;
            };

            std::function<bool(const std::string&, const std::string&, bool)> emitInspectorScope;
            emitInspectorScope = [&](const std::string& scopeText, const std::string& indent,
                                    bool insideTabs) -> bool {
                size_t i = 0;
                while (i < scopeText.size()) {
                    while (i < scopeText.size() &&
                        std::isspace(static_cast<unsigned char>(scopeText[i])) != 0) {
                        ++i;
                    }
                    if (i >= scopeText.size()) break;

                    if (!(std::isalpha(static_cast<unsigned char>(scopeText[i])) != 0 ||
                        scopeText[i] == '_')) {
                        error = "Unsupported inspector syntax near: " + trimCopy(scopeText.substr(i, 32));
                        return false;
                    }

                    const size_t nameStart = i;
                    ++i;
                    while (i < scopeText.size()) {
                        const unsigned char c = static_cast<unsigned char>(scopeText[i]);
                        if (std::isalnum(c) == 0 && scopeText[i] != '_') break;
                        ++i;
                    }
                    const std::string callName = scopeText.substr(nameStart, i - nameStart);

                    while (i < scopeText.size() &&
                        std::isspace(static_cast<unsigned char>(scopeText[i])) != 0) {
                        ++i;
                    }

                    std::vector<std::string> args;
                    if (i < scopeText.size() && scopeText[i] == '(') {
                        const size_t closeParen = findMatchingParen(scopeText, i);
                        if (closeParen == std::string::npos) {
                            error = "Unmatched '(' in inspector statement: " + callName;
                            return false;
                        }
                        args = splitTopLevel(scopeText.substr(i + 1, closeParen - i - 1), ',');
                        if (args.size() == 1 && trimCopy(args[0]).empty()) {
                            args.clear();
                        }
                        i = closeParen + 1;
                        while (i < scopeText.size() &&
                            std::isspace(static_cast<unsigned char>(scopeText[i])) != 0) {
                            ++i;
                        }
                    }

                    if (i < scopeText.size() && scopeText[i] == '{') {
                        const size_t closeBrace = findMatchingBrace(scopeText, i);
                        if (closeBrace == std::string::npos) {
                            error = "Unmatched '{' in inspector container: " + callName;
                            return false;
                        }
                        const std::string inner = scopeText.substr(i + 1, closeBrace - i - 1);

                        if (callName == "Tabs") {
                            if (!args.empty()) {
                                error = "inspector Tabs does not take arguments.";
                                return false;
                            }
                            out << indent << "if (ModuGUI::BeginTabBar(\"##ModuCPPInspectorTabs"
                                << ++inspectorTempCounter << "\")) {\n";
                            if (!emitInspectorScope(inner, indent + "    ", true)) {
                                return false;
                            }
                            out << indent << "    ModuGUI::EndTabBar();\n";
                            out << indent << "}\n";
                        } else if (callName == "Tab") {
                            if (args.size() != 1) {
                                error = "inspector Tab(title) requires exactly one argument.";
                                return false;
                            }
                            if (!insideTabs) {
                                error = "inspector Tab(...) must be inside Tabs { ... }.";
                                return false;
                            }
                            out << indent << "if (ModuGUI::BeginTabItem(" << trimCopy(args[0]) << ")) {\n";
                            if (!emitInspectorScope(inner, indent + "    ", false)) {
                                return false;
                            }
                            out << indent << "    ModuGUI::EndTabItem();\n";
                            out << indent << "}\n";
                        } else if (callName == "Section") {
                            if (args.size() != 1) {
                                error = "inspector Section(title) requires exactly one argument.";
                                return false;
                            }
                            out << indent << "ModuGUI::TextUnformatted(" << trimCopy(args[0]) << ");\n";
                            out << indent << "ModuGUI::Separator();\n";
                            if (!emitInspectorScope(inner, indent, insideTabs)) {
                                return false;
                            }
                        } else if (callName == "Group") {
                            if (!args.empty()) {
                                out << indent << "ModuGUI::TextUnformatted(" << trimCopy(args[0]) << ");\n";
                            }
                            out << indent << "ModuGUI::BeginGroup();\n";
                            if (!emitInspectorScope(inner, indent + "    ", insideTabs)) {
                                return false;
                            }
                            out << indent << "ModuGUI::EndGroup();\n";
                        } else if (callName == "Foldout") {
                            if (args.size() != 1) {
                                error = "inspector Foldout(title) requires exactly one argument.";
                                return false;
                            }
                            out << indent << "if (ModuGUI::SubsectionFoldout(" << trimCopy(args[0]) << ")) {\n";
                            if (!emitInspectorScope(inner, indent + "    ", insideTabs)) {
                                return false;
                            }
                            out << indent << "    ModuGUI::TreePop();\n";
                            out << indent << "}\n";
                        } else {
                            error = "Unsupported inspector container: " + callName;
                            return false;
                        }

                        i = closeBrace + 1;
                        while (i < scopeText.size() &&
                            std::isspace(static_cast<unsigned char>(scopeText[i])) != 0) {
                            ++i;
                        }
                        if (i < scopeText.size() && scopeText[i] == ';') {
                            ++i;
                        }
                        continue;
                    }

                    if (i >= scopeText.size() || scopeText[i] != ';') {
                        error = "Missing ';' after inspector statement: " + callName;
                        return false;
                    }
                    ++i;

                    if (!emitInspectorCall(callName, args, indent)) {
                        return false;
                    }
                }
                return true;
            };

            const std::string inspectorDsl = stripCommentsPreserveLayout(spec.inspectorBlock);
            if (!emitInspectorScope(inspectorDsl, "    ", false)) {
                return {};
            }

            for (const FieldSpec* field : inspectorSnapshotFields) {
                out << "    {\n";
                out << "        const std::string _moduAfter_" << field->name
                    << " = DialoguePort::SerializeDialogueLines(config." << field->name << ");\n";
                out << "        if (_moduAfter_" << field->name << " != _moduBefore_" << field->name << ") {\n";
                out << "            ctx.SetSetting(\"" << field->name << "\", _moduAfter_" << field->name << ");\n";
                out << "            changed = true;\n";
                out << "        }\n";
                out << "    }\n";
            }

            if (!hasExplicitInspectorSave) {
                out << "    if (changed) {\n";
                out << "        ctx.SaveAutoSettings();\n";
                out << "    }\n";
            }
            out << "}\n\n";
        } else if (hasAutoInspectorFields && !hasInspectorMethod) {
            out << "extern \"C\" MODULARITY_SCRIPT_EXPORT void Script_OnInspector(ScriptContext& ctx) {\n";
            out << "    MODU_SCRIPT(ctx);\n";
            out << "    auto& config = ::ModuCPP::Config<" << supportNs << "::" << configType << ">();\n";
            out << "    " << supportNs << "::BindConfig(ctx, config);\n";
            out << "    bool changed = false;\n";
            std::unordered_set<std::string> skippedInspectorFields;
            for (size_t fieldIndex = 0; fieldIndex < spec.fields.size(); ++fieldIndex) {
                if (!emitAutoInspectorField(fieldIndex, spec.fields[fieldIndex], skippedInspectorFields)) {
                    return {};
                }
            }
            out << "    if (changed) {\n";
            out << "        ctx.SaveAutoSettings();\n";
            out << "    }\n";
            out << "}\n\n";
        }

        // Forward-declare all methods so they can call each other regardless of definition order.
        for (const MethodSpec& method : spec.methods) {
            if (toLowerCopy(removeWhitespaceCopy(method.returnType)) == "auto") {
                continue;
            }
            if (method.isStatic) out << "static ";
            out << method.returnType << " " << method.name << "(" << method.transpiledParams << ");\n";
        }
        out << "\n";

        for (const MethodSpec& method : spec.methods) {
            const std::string preLoweredBody = lowerEachInSyntax(method.body);
            std::string rewrittenBody = cachedRewriteSurfaceSyntax(preLoweredBody);
            // Narrow rewrite for known [ObjectRef] field truthiness + UI access.
            // Runs before transformEachSyntax so the each-loop pass sees the same body.
            rewrittenBody = transformObjectRefAccess(rewrittenBody, objectRefFields);
            rewrittenBody = transformSpriteAssignment(rewrittenBody, objectRefFields);
            std::string transformedBody = transformEachSyntax(rewrittenBody, listFields, supportNs,
                                                            method.hasContext, error);
            if (!error.empty()) {
                return {};
            }

            if (method.isStatic) out << "static ";
            out << method.returnType << " " << method.name << "(" << method.transpiledParams << ") {\n";
            if (method.isCalc) {
                emitSourceLineDirective(method.bodySourceLine);
                out << transformedBody << "\n";
                emitGeneratedLineDirective();
                out << "}\n\n";
                continue;
            }
            if (method.isStatic) {
                out << transformedBody << "\n";
                out << "}\n\n";
                continue;
            }
            if (method.hasContext) {
                const bool manualPreludeMode = method.hasManualPrelude && spec.fields.empty();
                if (method.hasDeltaTimeParam) {
                    out << "    ::ModuCPP::SetFrameDeltaTime(" << method.deltaTimeParamName << ");\n";
                }
                if (!manualPreludeMode) {
                    if (method.contextIsPointer) {
                        if (isVoidReturnType(method.returnType)) {
                            out << "    if (!" << method.contextParamName << ") return;\n";
                        } else {
                            out << "    if (!" << method.contextParamName << ") return {};\n";
                        }
                        out << "    ScriptContext& ctx = *" << method.contextParamName << ";\n";
                    } else if (method.contextParamName != "ctx") {
                        out << "    ScriptContext& ctx = " << method.contextParamName << ";\n";
                    }
                    out << "    MODU_SCRIPT(ctx);\n";
                    if (!spec.fields.empty()) {
                        out << "    auto& config = ::ModuCPP::Config<" << supportNs << "::" << configType << ">();\n";
                        out << "    " << supportNs << "::BindConfig(ctx, config);\n";
                        for (const FieldSpec& field : spec.fields) {
                            if (!fieldPersists(field)) continue;
                            out << "    auto& " << field.name << " = config." << field.name << ";\n";
                        }
                        if (hasTransientFields) {
                            out << "    auto& state = ::ModuCPP::State<" << supportNs << "::" << stateType << ">();\n";
                            for (const FieldSpec& field : spec.fields) {
                                if (fieldPersists(field)) continue;
                                out << "    auto& " << field.name << " = state." << field.name << ";\n";
                            }
                        }
                    }
                }
            } else {
                out << "    ScriptContext* _moduCtxPtr = ::ModuCPP::ctxPtr();\n";
                if (isVoidReturnType(method.returnType)) {
                    out << "    if (!_moduCtxPtr) return;\n";
                } else {
                    out << "    if (!_moduCtxPtr) return {};\n";
                }
                out << "    ScriptContext& ctx = *_moduCtxPtr;\n";
                if (method.hasDeltaTimeParam) {
                    out << "    ::ModuCPP::SetFrameDeltaTime(" << method.deltaTimeParamName << ");\n";
                }
                out << "    MODU_SCRIPT(ctx);\n";
                if (!spec.fields.empty()) {
                    out << "    auto& config = ::ModuCPP::Config<" << supportNs << "::" << configType << ">();\n";
                    out << "    " << supportNs << "::BindConfig(ctx, config);\n";
                    for (const FieldSpec& field : spec.fields) {
                        if (!fieldPersists(field)) continue;
                        out << "    auto& " << field.name << " = config." << field.name << ";\n";
                    }
                    if (hasTransientFields) {
                        out << "    auto& state = ::ModuCPP::State<" << supportNs << "::" << stateType << ">();\n";
                        for (const FieldSpec& field : spec.fields) {
                            if (fieldPersists(field)) continue;
                            out << "    auto& " << field.name << " = state." << field.name << ";\n";
                        }
                    }
                }
            }
            // Wrap the user body in a nested scope when parameters were auto-injected
            // (e.g. 'float dt') so that re-declarations in the body (e.g. 'float dt = time.deltaTime')
            // don't shadow the injected parameters and cause a compile error.
            const bool needsBodyScope = method.autoInjectedContext || method.autoInjectedDeltaTime;
            emitSourceLineDirective(method.bodySourceLine);
            if (needsBodyScope) {
                out << "    {\n" << transformedBody << "\n    }\n";
            } else {
                out << transformedBody << "\n";
            }
            emitGeneratedLineDirective();
            out << "}\n\n";
        }

        auto findMethodByName = [&](const std::string& name) -> const MethodSpec* {
            for (const MethodSpec& method : spec.methods) {
                if (method.name == name) {
                    return &method;
                }
            }
            return nullptr;
        };
        if (const MethodSpec* method = findMethodByName("OnCollideEnter")) {
            out << "extern \"C\" MODULARITY_SCRIPT_EXPORT void Script_OnCollideEnter(ScriptContext& ctx, SceneObject* other) {\n";
            out << "    " << method->name << "(ctx, other, 0.0f);\n";
            out << "}\n\n";
        }
        if (const MethodSpec* method = findMethodByName("OnCollideHold")) {
            const std::string duration = method->collisionHoldDuration.empty() ? "2.0f" : method->collisionHoldDuration;
            out << "extern \"C\" MODULARITY_SCRIPT_EXPORT float Script_OnCollideHoldDuration() {\n";
            out << "    return " << duration << ";\n";
            out << "}\n\n";
            out << "extern \"C\" MODULARITY_SCRIPT_EXPORT void Script_OnCollideHold(ScriptContext& ctx, SceneObject* other) {\n";
            out << "    " << method->name << "(ctx, other, " << duration << ");\n";
            out << "}\n\n";
        }
        if (const MethodSpec* method = findMethodByName("OnCollideExit")) {
            out << "extern \"C\" MODULARITY_SCRIPT_EXPORT void Script_OnCollideExit(ScriptContext& ctx, SceneObject* other) {\n";
            out << "    " << method->name << "(ctx, other, 0.0f);\n";
            out << "}\n\n";
        }

        return out.str();
    }

    bool cBackendDisabledByEnv() {
        const char* v = std::getenv("MODUCPP_DISABLE_C_BACKEND");
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }

    bool cIsNumericLiteral(const std::string& value) {
        static const std::regex kNumber(R"(^[+-]?(\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?[fF]?$)");
        return std::regex_match(trimCopy(value), kNumber);
    }

    float cParseNumericLiteral(const std::string& value) {
        return static_cast<float>(std::strtod(trimCopy(value).c_str(), nullptr));
    }

    std::string cFormatFloatLiteral(float value) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.6gf", static_cast<double>(value));
        return buf;
    }

    bool cIsVec3TypeName(const std::string& name) {
        return name == "vec3" || name == "Vector3";
    }

    // Main-class fields the C++ backend draws with DrawObjectRefInput and stores as a reference
    // string ("Object.ID-<id>"): SceneObj-typed fields and [ObjectRef] strings.
    bool cFieldIsObjectRef(const FieldSpec& field) {
        if (!field.arrayDimensions.empty()) return false;
        return field.kind == FieldKind::ObjectRef ||
               (field.kind == FieldKind::String && field.hasObjectRefAttribute);
    }

    bool cMathNameSupported(const std::string& name) {
        static const std::unordered_set<std::string> kNames = {
            "Sin", "Cos", "Tan", "Sqrt", "Abs", "Floor", "Ceil", "Exp", "Pow",
            "Min", "Max", "Clamp", "Clamp01", "Lerp", "Length"
        };
        return kNames.find(name) != kNames.end();
    }

    bool cCtxMethodSupported(const std::string& name) {
        static const std::unordered_set<std::string> kNames = {
            "GetObjectId", "IsObjectEnabled", "SetObjectEnabled",
            "SetPosition", "SetRotation", "SetScale",
            "IsSprintDown", "IsJumpDown", "GetMoveInputWASD",
            "AddConsoleMessage",
            "GetSettingFloat", "SetSettingFloat", "GetSettingBool", "SetSettingBool",
            "HasAnimation", "PlayAnimation", "StopAnimation", "PauseAnimation",
            "ReverseAnimation", "SetAnimationTime", "GetAnimationTime",
            "IsAnimationPlaying", "SetAnimationLoop", "SetAnimationPlaySpeed",
            "SetAnimationPlayOnAwake",
            "GetSpriteClipCount", "GetSpriteClipIndex", "SetSpriteClipIndex",
            "EnsureRigidbody", "EnsureCapsuleCollider",
            "SetRigidbodyVelocity", "AddRigidbodyForce", "SetRigidbodyRotation",
            "GetProjectGravityScale", "SetProjectGravityScale"
        };
        return kNames.find(name) != kNames.end();
    }

    bool cKeywordAllowed(const std::string& word) {
        static const std::unordered_set<std::string> kWords = {
            "if", "else", "for", "while", "do", "return", "break", "continue",
            "switch", "case", "default", "const", "static", "void", "sizeof",
            "float", "double", "int", "bool", "unsigned", "long", "short", "char",
            "true", "false", "NULL"
        };
        return kWords.find(word) != kWords.end();
    }

    struct CBodyLowerer {
        const std::string& src;
        const std::unordered_map<std::string, const FieldSpec*>& publicFields;
        const std::unordered_map<std::string, const FieldSpec*>& privateFields;
        const std::unordered_set<std::string>& helperNames;
        bool hasCtx = false;
        bool hasDt = false;
        std::string dtName = "dt";

        std::string out;
        std::string reason;
        std::set<std::string> usedPublics;
        bool usedState = false;

        size_t i = 0;
        char prevSig = '{';
        bool atStmtStart = true;
        std::vector<std::string> parenInserts;
        std::string closeBeforeSemi;
        std::string suffixAfterSemi;
        std::unordered_map<std::string, char> locals;

        CBodyLowerer(const std::string& body,
                     const std::unordered_map<std::string, const FieldSpec*>& pub,
                     const std::unordered_map<std::string, const FieldSpec*>& priv,
                     const std::unordered_set<std::string>& helpers)
            : src(body), publicFields(pub), privateFields(priv), helperNames(helpers) {}

        bool fail(const std::string& r) {
            if (reason.empty()) reason = r;
            return false;
        }

        static bool isWs(char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        size_t peekSig(size_t from) const {
            size_t p = from;
            while (p < src.size()) {
                if (isWs(src[p])) { ++p; continue; }
                if (src[p] == '/' && p + 1 < src.size() && src[p + 1] == '/') {
                    while (p < src.size() && src[p] != '\n') ++p;
                    continue;
                }
                if (src[p] == '/' && p + 1 < src.size() && src[p + 1] == '*') {
                    p += 2;
                    while (p + 1 < src.size() && !(src[p] == '*' && src[p + 1] == '/')) ++p;
                    p = (p + 1 < src.size()) ? p + 2 : src.size();
                    continue;
                }
                return p;
            }
            return std::string::npos;
        }

        std::string peekWord(size_t at) const {
            if (at == std::string::npos || at >= src.size() || !isIdentifierStartChar(src[at])) return {};
            size_t e = at;
            while (e < src.size() && isIdentifierChar(src[e])) ++e;
            return src.substr(at, e - at);
        }

        std::string peekAssignOp(size_t at) const {
            if (at == std::string::npos || at >= src.size()) return {};
            const char c = src[at];
            const char n = (at + 1 < src.size()) ? src[at + 1] : '\0';
            if (c == '=' && n != '=') return "=";
            if ((c == '+' || c == '-') && n == c) return std::string(2, c);
            if ((c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                 c == '&' || c == '|' || c == '^') && n == '=') {
                return std::string(1, c) + "=";
            }
            return {};
        }

        static bool isArithChar(char c) {
            return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                   c == '<' || c == '>' || c == '!' || c == '&' || c == '|' || c == '^';
        }

        bool checkVec3Context(size_t afterPos) {
            if (isArithChar(prevSig)) {
                return fail("vec3 arithmetic needs the C++ backend");
            }
            const size_t nxt = peekSig(afterPos);
            if (nxt != std::string::npos) {
                const char c = src[nxt];
                if (isArithChar(c)) return fail("vec3 arithmetic needs the C++ backend");
                if (c == '=' && nxt + 1 < src.size() && src[nxt + 1] == '=') {
                    return fail("vec3 comparison needs the C++ backend");
                }
            }
            return true;
        }

        void emitSig(const std::string& text, char sig) {
            out += text;
            prevSig = sig;
            atStmtStart = false;
        }

        int countCallArgs(size_t openAt) const {
            if (openAt == std::string::npos || openAt >= src.size() || src[openAt] != '(') return -1;
            int depth = 0;
            int commas = 0;
            bool sawContent = false;
            for (size_t p = openAt; p < src.size(); ++p) {
                const char c = src[p];
                if (c == '"' || c == '\'') {
                    const char quote = c;
                    ++p;
                    while (p < src.size() && src[p] != quote) {
                        if (src[p] == '\\') ++p;
                        ++p;
                    }
                    sawContent = true;
                    continue;
                }
                if (c == '(') { ++depth; continue; }
                if (c == ')') {
                    --depth;
                    if (depth == 0) return sawContent ? commas + 1 : 0;
                    continue;
                }
                if (depth == 1 && c == ',') { ++commas; continue; }
                if (depth >= 1 && !isWs(c)) sawContent = true;
            }
            return -1;
        }

        bool consumeCallOpen(const std::string& insertBeforeClose) {
            const size_t open = peekSig(i);
            if (open == std::string::npos || src[open] != '(') {
                return fail("expected '(' after rewritten call");
            }
            i = open + 1;
            parenInserts.push_back(insertBeforeClose);
            return true;
        }

        bool lowerTransformAccess();
        bool lowerIdentifier(const std::string& word);
        bool run();
    };

    bool CBodyLowerer::lowerTransformAccess() {
        size_t p = peekSig(i);
        if (p == std::string::npos || src[p] != '.') return fail("only obj.Transform.* is supported on obj");
        p = peekSig(p + 1);
        const std::string node = peekWord(p);
        if (node != "Transform") return fail("only obj.Transform.* is supported on obj");
        p = peekSig(p + node.size());
        if (p == std::string::npos || src[p] != '.') return fail("only obj.Transform.* is supported on obj");
        p = peekSig(p + 1);
        const std::string prop = peekWord(p);
        std::string getter, setter;
        if (prop == "position") { getter = "Modu_GetPosition"; setter = "Modu_SetPosition"; }
        else if (prop == "rotation") { getter = "Modu_GetRotation"; setter = "Modu_SetRotation"; }
        else if (prop == "scale") { getter = "Modu_GetScale"; setter = "Modu_SetScale"; }
        else return fail("obj.Transform." + prop + " has no C lowering yet");
        const bool wasStmtStart = atStmtStart;
        i = p + prop.size();

        size_t after = peekSig(i);
        if (after != std::string::npos && src[after] == '.') {
            const size_t memberPos = peekSig(after + 1);
            const std::string member = peekWord(memberPos);
            if (member != "x" && member != "y" && member != "z") {
                return fail("unknown transform member '." + member + "'");
            }
            const size_t afterMember = memberPos + member.size();
            const std::string op = peekAssignOp(peekSig(afterMember));
            if (!op.empty()) {
                if (!wasStmtStart) return fail("transform writes must be plain statements");
                if (!closeBeforeSemi.empty() || !suffixAfterSemi.empty()) {
                    return fail("chained transform writes in one statement");
                }
                if (op == "++" || op == "--") {
                    i = peekSig(afterMember) + 2;
                    out += "{ ModuVec3 _moduTmp = " + getter + "(ctx); " + op +
                           "_moduTmp." + member;
                    suffixAfterSemi = " " + setter + "(ctx, _moduTmp); }";
                    prevSig = 'a';
                    atStmtStart = false;
                    return true;
                }
                i = peekSig(afterMember) + op.size();
                out += "{ ModuVec3 _moduTmp = " + getter + "(ctx); _moduTmp." +
                       member + " " + op;
                suffixAfterSemi = " " + setter + "(ctx, _moduTmp); }";
                prevSig = '=';
                atStmtStart = false;
                return true;
            }
            i = afterMember;
            emitSig(getter + "(ctx)." + member, 'a');
            return true;
        }

        const std::string op = peekAssignOp(after);
        if (!op.empty()) {
            if (op != "=") return fail("vec3 arithmetic needs the C++ backend");
            if (!wasStmtStart) return fail("transform writes must be plain statements");
            if (!closeBeforeSemi.empty() || !suffixAfterSemi.empty()) {
                return fail("chained transform writes in one statement");
            }
            i = after + 1;
            emitSig(setter + "(ctx, ", ' ');
            closeBeforeSemi = ")";
            return true;
        }

        if (!checkVec3Context(i)) return false;
        emitSig(getter + "(ctx)", 'a');
        return true;
    }

    bool CBodyLowerer::lowerIdentifier(const std::string& word) {
        const size_t after = i;

        if (cKeywordAllowed(word)) {
            const bool isType = (word == "float" || word == "int" || word == "bool" || word == "double");
            if (isType) {
                const std::string next = peekWord(peekSig(after));
                if (!next.empty()) {
                    if (publicFields.count(next) || privateFields.count(next) || helperNames.count(next)) {
                        return fail("local '" + next + "' shadows a field or method");
                    }
                    locals[next] = (word == "bool") ? 'b' : (word == "int" ? 'i' : 'f');
                }
            }
            emitSig(word, word.back());
            if (word == "else" || word == "do" || word == "return") atStmtStart = (word != "return");
            return true;
        }

        if (cIsVec3TypeName(word)) {
            const size_t nxt = peekSig(after);
            if (nxt != std::string::npos && src[nxt] == '(') {
                const int args = countCallArgs(nxt);
                if (args == 1) { emitSig("ModuC_Vec3Splat", 't'); return true; }
                if (args == 3) { emitSig("ModuC_Vec3", '3'); return true; }
                return fail("vec3(...) needs 1 or 3 arguments");
            }
            const std::string next = peekWord(nxt);
            if (!next.empty()) {
                if (publicFields.count(next) || privateFields.count(next) || helperNames.count(next)) {
                    return fail("local '" + next + "' shadows a field or method");
                }
                locals[next] = 'v';
                emitSig("ModuVec3", '3');
                return true;
            }
            return fail("unsupported vec3 usage");
        }

        if (word == "Math") {
            size_t p = peekSig(after);
            if (p == std::string::npos || src[p] != '.') return fail("bare 'Math' is not lowerable");
            p = peekSig(p + 1);
            const std::string fn = peekWord(p);
            if (!cMathNameSupported(fn)) return fail("Math." + fn + " has no C lowering yet");
            i = p + fn.size();
            emitSig("ModuC_Math_" + fn, 'h');
            return true;
        }

        if (word == "Type") {
            size_t p = peekSig(after);
            if (p == std::string::npos || src[p] != '.') return fail("bare 'Type' is not lowerable");
            p = peekSig(p + 1);
            const std::string level = peekWord(p);
            std::string lowered;
            if (level == "Info") lowered = "MODU_CONSOLE_INFO";
            else if (level == "Warning") lowered = "MODU_CONSOLE_WARNING";
            else if (level == "Error") lowered = "MODU_CONSOLE_ERROR";
            else if (level == "Success") lowered = "MODU_CONSOLE_SUCCESS";
            else return fail("Type." + level + " is not a console level");
            i = p + level.size();
            emitSig(lowered, 'S');
            return true;
        }

        if (word == "AddLog") {
            if (!hasCtx) return fail("AddLog outside a lifecycle method");
            const size_t open = peekSig(after);
            const int args = countCallArgs(open);
            if (args != 1 && args != 2) return fail("AddLog needs 1 or 2 arguments");
            emitSig("Modu_AddConsoleMessage", 'e');
            if (!consumeCallOpen(args == 1 ? ", MODU_CONSOLE_INFO" : "")) return false;
            out += "(ctx, ";
            prevSig = ' ';
            return true;
        }

        if (word == "ctx") {
            if (!hasCtx) return fail("ctx in a context-free helper");
            size_t p = peekSig(after);
            if (p == std::string::npos || src[p] != '.') return fail("bare 'ctx' is not lowerable");
            p = peekSig(p + 1);
            const std::string fn = peekWord(p);
            if (!cCtxMethodSupported(fn)) return fail("ctx." + fn + " has no C lowering yet");
            i = p + fn.size();
            emitSig("Modu_" + fn, 'x');
            const size_t open = peekSig(i);
            const int args = countCallArgs(open);
            if (args < 0) return fail("expected a call after ctx." + fn);
            if (!consumeCallOpen("")) return false;
            out += (args == 0) ? "(ctx" : "(ctx, ";
            prevSig = (args == 0) ? 'x' : ' ';
            return true;
        }

        if (word == "obj") {
            if (!hasCtx) return fail("obj in a context-free helper");
            return lowerTransformAccess();
        }

        if (word == dtName) {
            if (!hasDt) return fail("'" + word + "' is not available here");
            emitSig(word, 't');
            return true;
        }

        if (word.rfind("_modu", 0) == 0) {
            return fail("identifier '" + word + "' collides with the reserved _modu prefix");
        }

        const auto priv = privateFields.find(word);
        if (priv != privateFields.end()) {
            if (!hasCtx) return fail("field '" + word + "' used in a context-free helper");
            usedState = true;
            if (priv->second->kind == FieldKind::Vec3) {
                const size_t nxt = peekSig(after);
                const bool memberAccess = (nxt != std::string::npos && src[nxt] == '.');
                if (!memberAccess && !checkVec3Context(after)) return false;
            }
            emitSig("_moduState->" + word, 'd');
            return true;
        }

        const auto pub = publicFields.find(word);
        if (pub != publicFields.end()) {
            if (!hasCtx) return fail("field '" + word + "' used in a context-free helper");
            const std::string op = peekAssignOp(peekSig(after));
            if (!op.empty() || prevSig == '#') {
                return fail("public field '" + word + "' is written to (read-only in the C backend)");
            }
            const FieldSpec& field = *pub->second;
            if (cFieldIsObjectRef(field)) {
                return fail("ObjectRef field '" + word + "' is used in code (needs the object-by-id API)");
            }
            const size_t nxt = peekSig(after);
            const char nextSig = (nxt == std::string::npos) ? '\0' : src[nxt];
            if (field.kind == FieldKind::String) {
                // A C char buffer: fine to hand to a call, wrong for +, == and the like.
                // ' ' is what the call rewrites (AddLog -> Modu_AddConsoleMessage(ctx, ...) leave behind.
                const bool callArgument = (prevSig == '(' || prevSig == ',' || prevSig == ' ') &&
                                          (nextSig == ')' || nextSig == ',');
                if (!callArgument) {
                    return fail("string field '" + word + "' is used outside a call argument");
                }
            } else if (field.kind == FieldKind::Vec3 && nextSig != '.' && !checkVec3Context(after)) {
                return false;
            }
            usedPublics.insert(word);
            emitSig(word, 'p');
            return true;
        }

        if (helperNames.count(word)) {
            emitSig(word, 'm');
            return true;
        }

        const auto local = locals.find(word);
        if (local != locals.end()) {
            if (local->second == 'v') {
                const size_t nxt = peekSig(after);
                const bool memberAccess = (nxt != std::string::npos && src[nxt] == '.');
                const std::string op = peekAssignOp(nxt);
                if (!op.empty() && op != "=") return fail("vec3 arithmetic needs the C++ backend");
                if (!memberAccess && op.empty() && !checkVec3Context(after)) return false;
            }
            emitSig(word, 'l');
            return true;
        }

        if (word.rfind("Modu_", 0) == 0 || word.rfind("ModuC_", 0) == 0 ||
            word.rfind("MODU_", 0) == 0 || word.rfind("MODUC_", 0) == 0 ||
            word == "ModuVec3") {
            emitSig(word, 'M');
            return true;
        }

        return fail("identifier '" + word + "' has no C lowering yet");
    }

    bool CBodyLowerer::run() {
        while (i < src.size()) {
            const char c = src[i];

            if (isWs(c)) { out += c; ++i; continue; }

            if (c == '/' && i + 1 < src.size() && (src[i + 1] == '/' || src[i + 1] == '*')) {
                const size_t start = i;
                if (src[i + 1] == '/') {
                    while (i < src.size() && src[i] != '\n') ++i;
                } else {
                    i += 2;
                    while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
                    i = (i + 1 < src.size()) ? i + 2 : src.size();
                }
                out += src.substr(start, i - start);
                continue;
            }

            if (c == '"' || c == '\'') {
                if (prevSig == '+') return fail("string concatenation needs the C++ backend");
                const size_t start = i;
                ++i;
                while (i < src.size() && src[i] != c) {
                    if (src[i] == '\\') ++i;
                    ++i;
                }
                if (i < src.size()) ++i;
                out += src.substr(start, i - start);
                prevSig = c;
                atStmtStart = false;
                const size_t nxt = peekSig(i);
                if (nxt != std::string::npos && src[nxt] == '+') {
                    return fail("string concatenation needs the C++ backend");
                }
                continue;
            }

            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
                const size_t start = i;
                while (i < src.size() &&
                       (isIdentifierChar(src[i]) || src[i] == '.' ||
                        ((src[i] == '+' || src[i] == '-') && i > start &&
                         (src[i - 1] == 'e' || src[i - 1] == 'E')))) {
                    ++i;
                }
                out += src.substr(start, i - start);
                prevSig = '0';
                atStmtStart = false;
                continue;
            }

            if (isIdentifierStartChar(c)) {
                size_t e = i;
                while (e < src.size() && isIdentifierChar(src[e])) ++e;
                const std::string word = src.substr(i, e - i);
                i = e;
                if (!lowerIdentifier(word)) return false;
                continue;
            }

            if (c == '(') { parenInserts.push_back(std::string()); out += c; ++i; prevSig = '('; atStmtStart = false; continue; }
            if (c == ')') {
                if (!parenInserts.empty()) {
                    out += parenInserts.back();
                    parenInserts.pop_back();
                }
                out += c;
                ++i;
                prevSig = ')';
                atStmtStart = parenInserts.empty();
                continue;
            }
            if (c == ';') {
                if (parenInserts.empty()) {
                    out += closeBeforeSemi;
                    out += ';';
                    out += suffixAfterSemi;
                    closeBeforeSemi.clear();
                    suffixAfterSemi.clear();
                    atStmtStart = true;
                } else {
                    out += ';';
                }
                ++i;
                prevSig = ';';
                continue;
            }
            if (c == '{' || c == '}') {
                if (!closeBeforeSemi.empty() || !suffixAfterSemi.empty()) {
                    return fail("unexpected brace inside a transform write");
                }
                out += c;
                ++i;
                prevSig = c;
                atStmtStart = true;
                continue;
            }
            if (c == '.') {
                const size_t memberPos = peekSig(i + 1);
                const std::string member = peekWord(memberPos);
                if (member == "x" || member == "y" || member == "z") {
                    out += '.';
                    out += member;
                    i = memberPos + member.size();
                    prevSig = 'a';
                    atStmtStart = false;
                    continue;
                }
                return fail("member '." + member + "' has no C lowering yet");
            }
            if (c == ':' && i + 1 < src.size() && src[i + 1] == ':') return fail("'::' needs the C++ backend");
            if (c == '-' && i + 1 < src.size() && src[i + 1] == '>') return fail("'->' needs the C++ backend");
            if (c == '[' || c == ']') return fail("arrays and lambdas need the C++ backend");
            if (c == '#') return fail("preprocessor directives inside bodies");

            if ((c == '+' || c == '-') && i + 1 < src.size() && src[i + 1] == c) {
                const std::string target = peekWord(peekSig(i + 2));
                if (publicFields.count(target)) {
                    return fail("public field '" + target + "' is written to (read-only in the C backend)");
                }
                out += c;
                out += c;
                i += 2;
                prevSig = c;
                atStmtStart = false;
                continue;
            }

            out += c;
            ++i;
            prevSig = c;
            atStmtStart = false;
        }
        if (!parenInserts.empty()) return fail("unbalanced parentheses");
        if (!closeBeforeSemi.empty() || !suffixAfterSemi.empty()) {
            return fail("transform write not closed by ';'");
        }
        return true;
    }

    bool cIsStringLiteral(const std::string& value) {
        static const std::regex kStringLiteral(R"(^"([^"\\]|\\.)*"$)");
        return std::regex_match(trimCopy(value), kStringLiteral);
    }

    bool cIsIntegerLiteral(const std::string& value) {
        static const std::regex kInteger(R"(^[+-]?\d+$)");
        return std::regex_match(trimCopy(value), kInteger);
    }

    bool cVec3InitializerOk(const FieldSpec& field, std::string& reason) {
        const std::string init = trimCopy(field.initializer);
        if (init.empty()) return true;
        static const std::regex kVecInit(R"(^(?:vec3|Vector3)\s*\((.*)\)$)");
        std::smatch m;
        if (!std::regex_match(init, m, kVecInit)) {
            reason = "field '" + field.name + "' has a non-literal vec3 initializer";
            return false;
        }
        std::istringstream args(m[1].str());
        std::string part;
        int count = 0;
        while (std::getline(args, part, ',')) {
            if (!cIsNumericLiteral(part)) {
                reason = "field '" + field.name + "' has a non-literal vec3 initializer";
                return false;
            }
            ++count;
        }
        if (count != 1 && count != 3) {
            reason = "field '" + field.name + "' vec3 initializer needs 1 or 3 args";
            return false;
        }
        return true;
    }

    bool cFieldEligible(const FieldSpec& field, std::string& reason) {
        if (!field.arrayDimensions.empty()) { reason = "array field '" + field.name + "'"; return false; }
        if (field.hasObjectListAttribute || field.hasDialogueLinesAttribute ||
            field.hasClipGridPairAttribute || field.soundSetLabel ||
            (field.hasObjectRefAttribute && field.kind != FieldKind::String)) {
            reason = "field '" + field.name + "' uses attributes without a C lowering";
            return false;
        }
        const std::string init = trimCopy(field.initializer);
        if (field.visibility == FieldVisibility::Public || field.hasInspectorMetadata) {
            if (field.visibility != FieldVisibility::Public) {
                reason = "private field '" + field.name + "' with inspector metadata";
                return false;
            }
            // Attribute arguments are pasted into the generated C, so they must be literals.
            for (const std::optional<std::string>* expr : {&field.sliderMinExpr, &field.sliderMaxExpr,
                                                            &field.rangeMinExpr, &field.rangeMaxExpr,
                                                            &field.stepExpr}) {
                if (expr->has_value() && !cIsNumericLiteral(**expr)) {
                    reason = "field '" + field.name + "' has a non-literal attribute bound";
                    return false;
                }
            }
            if (field.hasSliderAttribute && (!field.sliderMinExpr || !field.sliderMaxExpr)) {
                reason = "field '" + field.name + "' slider bounds are not literals";
                return false;
            }
            if (field.inspectorHeader && !cIsStringLiteral(*field.inspectorHeader)) {
                reason = "field '" + field.name + "' header is not a plain string";
                return false;
            }
            const bool numeric = field.kind == FieldKind::Float || field.kind == FieldKind::Int ||
                                 field.kind == FieldKind::Vec3;
            if (field.hasSliderAttribute && !numeric) {
                reason = "[Slider] on non-numeric field '" + field.name + "'";
                return false;
            }
            if (cFieldIsObjectRef(field) || field.kind == FieldKind::String) {
                if (!init.empty() && !cIsStringLiteral(init)) {
                    reason = "field '" + field.name + "' has a non-literal initializer";
                    return false;
                }
                return true;
            }
            switch (field.kind) {
            case FieldKind::Float:
                if (!init.empty() && !cIsNumericLiteral(init)) {
                    reason = "field '" + field.name + "' has a non-literal initializer";
                    return false;
                }
                return true;
            case FieldKind::Int:
                if (!init.empty() && !cIsIntegerLiteral(init)) {
                    reason = "field '" + field.name + "' has a non-literal initializer";
                    return false;
                }
                return true;
            case FieldKind::Bool:
                if (!init.empty() && init != "true" && init != "false") {
                    reason = "field '" + field.name + "' has a non-literal initializer";
                    return false;
                }
                return true;
            case FieldKind::Vec3:
                return cVec3InitializerOk(field, reason);
            default:
                reason = "public field '" + field.name + "' (" + trimCopy(field.rawType) + ") has no C lowering";
                return false;
            }
        }
        switch (field.kind) {
        case FieldKind::Float:
        case FieldKind::Int:
        case FieldKind::Bool:
            if (!init.empty() && !cIsNumericLiteral(init) && init != "true" && init != "false") {
                reason = "field '" + field.name + "' has a non-literal initializer";
                return false;
            }
            return true;
        case FieldKind::Vec3:
            return cVec3InitializerOk(field, reason);
        default:
            reason = "field '" + field.name + "' type has no C lowering";
            return false;
        }
    }

    std::string cVec3InitExpr(const std::string& initializer) {
        const std::string init = trimCopy(initializer);
        if (init.empty()) return "ModuC_Vec3Splat(0.0f)";
        static const std::regex kVecInit(R"(^(?:vec3|Vector3)\s*\((.*)\)$)");
        std::smatch m;
        std::regex_match(init, m, kVecInit);
        const std::string args = trimCopy(m[1].str());
        return (args.find(',') == std::string::npos)
            ? "ModuC_Vec3Splat(" + args + ")"
            : "ModuC_Vec3(" + args + ")";
    }

    std::string cScalarInitExpr(const FieldSpec& field) {
        const std::string init = trimCopy(field.initializer);
        if (field.kind == FieldKind::Bool) return (init == "true") ? "1" : "0";
        if (init.empty()) return (field.kind == FieldKind::Int) ? "0" : "0.0f";
        return init;
    }

    bool generateTranspiledCSource(const fs::path& sourcePath, const ClassSpec& spec, std::string& outSource, std::string& reason) {
        if (!spec.subScripts.empty()) { reason = "sub-script structs"; return false; }
        if (!trimCopy(spec.inspectorBlock).empty()) { reason = "custom inspector block"; return false; }
        if (!spec.usingDirectives.empty()) { reason = "using directives"; return false; }

        static const std::unordered_set<std::string> kApiIncludeTargets = {
            "ModuCPP", "ModuEngine", "ModuInput", "RMeshBuilder", "ModuCPP.Experimental",
            "ModuCPPScriptApi.h", "ModuEngineScriptApi.h", "ModuInputScriptApi.h",
            "RMeshBuilderScriptApi.h", "ModuCPPExperimentalScriptApi.h"
        };
        for (const std::string& inc : spec.includeDirectives) {
            static const std::regex kIncludeTarget(R"(^\s*#include\s*[<"]([^>"]+)[>"]\s*$)");
            std::smatch m;
            if (!std::regex_match(inc, m, kIncludeTarget) ||
                kApiIncludeTargets.find(trimCopy(m[1].str())) == kApiIncludeTargets.end()) {
                reason = "include '" + inc + "'";
                return false;
            }
        }

        {
            std::istringstream input(stripCommentsPreserveLayout(spec.passthroughCode));
            std::string line;
            while (std::getline(input, line)) {
                const std::string trimmed = trimCopy(line);
                if (trimmed.empty()) continue;
                if (trimmed.rfind("#include", 0) == 0) continue;
                reason = "top-level code outside the class";
                return false;
            }
        }

        std::unordered_map<std::string, const FieldSpec*> publicFields;
        std::unordered_map<std::string, const FieldSpec*> privateFields;
        std::vector<const FieldSpec*> publicOrder;
        std::vector<const FieldSpec*> privateOrder;
        for (const FieldSpec& field : spec.fields) {
            if (!cFieldEligible(field, reason)) return false;
            if (field.visibility == FieldVisibility::Public) {
                publicFields[field.name] = &field;
                publicOrder.push_back(&field);
            } else {
                privateFields[field.name] = &field;
                privateOrder.push_back(&field);
            }
        }

        struct LoweredMethod {
            const MethodSpec* method = nullptr;
            std::string exportName;
            std::string returnType;
            std::string params;
            bool wantsDt = false;
        };
        std::vector<LoweredMethod> lifecycle;
        std::vector<LoweredMethod> helpers;
        std::unordered_set<std::string> helperNames;

        for (const MethodSpec& method : spec.methods) {
            if (method.isCalc) { reason = "calc method '" + method.name + "'"; return false; }
            if (isCollisionMethodName(method.name)) { reason = "collision hook '" + method.name + "'"; return false; }
            LoweredMethod lowered;
            lowered.method = &method;
            lowered.wantsDt = method.hasDeltaTimeParam || method.autoInjectedDeltaTime;
            if (method.name == "Begin") lowered.exportName = "Modu_Begin";
            else if (method.name == "TickUpdate") lowered.exportName = "Modu_TickUpdate";
            else if (method.name == "Update") lowered.exportName = "Modu_Update";
            else if (isLifecycleMethodName(method.name)) {
                reason = "lifecycle hook '" + method.name + "' has no C hook";
                return false;
            }

            if (!lowered.exportName.empty()) {
                std::string rest = method.originalParams;
                if (!trimCopy(rest).empty()) {
                    std::istringstream params(rest);
                    std::string part;
                    while (std::getline(params, part, ',')) {
                        const std::string p = trimCopy(part);
                        if (p.empty()) continue;
                        if (p.find("ScriptContext") != std::string::npos) continue;
                        if (looksLikeDeltaTimeParameter(p)) continue;
                        reason = "lifecycle '" + method.name + "' has extra parameters";
                        return false;
                    }
                }
                if (!isVoidReturnType(method.returnType)) {
                    reason = "lifecycle '" + method.name + "' returns a value";
                    return false;
                }
                lifecycle.push_back(lowered);
                continue;
            }

            if (method.hasContext) { reason = "helper '" + method.name + "' takes ctx"; return false; }
            std::string ret = trimCopy(method.returnType);
            if (cIsVec3TypeName(ret)) ret = "ModuVec3";
            else if (ret != "void" && ret != "float" && ret != "int" && ret != "bool" && ret != "double") {
                reason = "helper '" + method.name + "' return type '" + ret + "'";
                return false;
            }
            lowered.returnType = ret;
            std::string loweredParams;
            const std::string raw = trimCopy(method.originalParams);
            if (!raw.empty()) {
                std::istringstream params(raw);
                std::string part;
                while (std::getline(params, part, ',')) {
                    std::string p = trimCopy(part);
                    if (p.rfind("const ", 0) == 0) p = trimCopy(p.substr(6));
                    const size_t space = p.find_last_of(" \t");
                    if (space == std::string::npos) { reason = "helper '" + method.name + "' has an untyped parameter"; return false; }
                    std::string type = trimCopy(p.substr(0, space));
                    const std::string name = trimCopy(p.substr(space + 1));
                    if (cIsVec3TypeName(type)) type = "ModuVec3";
                    else if (type != "float" && type != "int" && type != "bool" && type != "double") {
                        reason = "helper '" + method.name + "' parameter type '" + type + "'";
                        return false;
                    }
                    if (!loweredParams.empty()) loweredParams += ", ";
                    loweredParams += type + " " + name;
                }
            }
            lowered.params = loweredParams.empty() ? "void" : loweredParams;
            helperNames.insert(method.name);
            helpers.push_back(lowered);
        }

        if (lifecycle.empty()) { reason = "no Begin/TickUpdate/Update hook"; return false; }

        const std::string san = sanitizeIdentifier(spec.name);
        const std::string stateType = san + "State";

        struct LoweredBody {
            std::string text;
            std::set<std::string> usedPublics;
            bool usedState = false;
        };
        std::vector<LoweredBody> lifecycleBodies(lifecycle.size());
        std::vector<LoweredBody> helperBodies(helpers.size());

        auto lowerBody = [&](const MethodSpec& method, bool wantsDt, LoweredBody& outBody) -> bool {
            CBodyLowerer lowerer(method.body, publicFields, privateFields, helperNames);
            lowerer.hasCtx = true;
            lowerer.hasDt = wantsDt;
            lowerer.dtName = method.deltaTimeParamName.empty() ? "dt" : method.deltaTimeParamName;
            if (!lowerer.run()) {
                reason = "method '" + method.name + "': " + lowerer.reason;
                return false;
            }
            outBody.text = std::move(lowerer.out);
            outBody.usedPublics = std::move(lowerer.usedPublics);
            outBody.usedState = lowerer.usedState;
            return true;
        };

        for (size_t idx = 0; idx < helpers.size(); ++idx) {
            CBodyLowerer lowerer(helpers[idx].method->body, publicFields, privateFields, helperNames);
            lowerer.hasCtx = false;
            lowerer.hasDt = false;
            if (helpers[idx].params != "void") {
                std::istringstream params(helpers[idx].params);
                std::string part;
                while (std::getline(params, part, ',')) {
                    std::string p = trimCopy(part);
                    const size_t space = p.find_last_of(' ');
                    const std::string type = p.substr(0, space);
                    const std::string name = p.substr(space + 1);
                    lowerer.locals[name] = (type == "ModuVec3") ? 'v' : (type == "int" ? 'i' : (type == "bool" ? 'b' : 'f'));
                }
            }
            if (!lowerer.run()) {
                reason = "method '" + helpers[idx].method->name + "': " + lowerer.reason;
                return false;
            }
            helperBodies[idx].text = std::move(lowerer.out);
        }

        for (size_t idx = 0; idx < lifecycle.size(); ++idx) {
            if (!lowerBody(*lifecycle[idx].method, lifecycle[idx].wantsDt, lifecycleBodies[idx])) {
                return false;
            }
        }

        const bool anyState = std::any_of(lifecycleBodies.begin(), lifecycleBodies.end(), [](const LoweredBody& b) { return b.usedState; });

        std::ostringstream out;
        out << "// generated by the ModuCPP transpiler (C backend) from "
            << sourcePath.filename().string() << ". do not edit by hand,\n"
            << "// the transpiler will happily stomp this on the next compile.\n"
            << "//\n"
            << "// this script qualified for the fast C path: tiny include surface,\n"
            << "// compiles in milliseconds. if the source grows a construct the C\n"
            << "// backend does not know, the transpiler quietly falls back to the\n"
            << "// classic C++ output. nothing breaks either way.\n"
            << "#include \"ModuCPPCSupport.h\"\n\n"
            << "#ifdef __cplusplus\n"
            << "#define MODU_HOOK_ABI extern \"C\"\n"
            << "#else\n"
            << "#define MODU_HOOK_ABI\n"
            << "#endif\n\n";

        if (anyState || !privateOrder.empty()) {
            out << "// per-object state (the class's private fields). slots are keyed by\n"
                << "// object id; if more than MODUC_MAX_STATE_SLOTS objects run this script\n"
                << "// the extras share the last slot, so bump that before shipping a horde.\n"
                << "typedef struct " << stateType << " {\n";
            for (const FieldSpec* field : privateOrder) {
                const char* type = "float";
                if (field->kind == FieldKind::Int) type = "int";
                else if (field->kind == FieldKind::Bool) type = "int";
                else if (field->kind == FieldKind::Vec3) type = "ModuVec3";
                out << "    " << type << " " << field->name << ";\n";
            }
            out << "} " << stateType << ";\n\n";
            // File scope rather than inside _GetState so Modu_ResetState below can clear it.
            out << "static struct { int objectId; int used; " << stateType << " state; } s_slots[MODUC_MAX_STATE_SLOTS];\n\n"
                << "static " << stateType << "* " << san << "_GetState(ModuScriptContext* ctx) {\n"
                << "    const int id = Modu_GetObjectId(ctx);\n"
                << "    int n;\n"
                << "    for (n = 0; n < MODUC_MAX_STATE_SLOTS; ++n) {\n"
                << "        if (s_slots[n].used && s_slots[n].objectId == id) return &s_slots[n].state;\n"
                << "        if (!s_slots[n].used) break;\n"
                << "    }\n"
                << "    if (n >= MODUC_MAX_STATE_SLOTS) n = MODUC_MAX_STATE_SLOTS - 1;\n"
                << "    s_slots[n].used = 1;\n"
                << "    s_slots[n].objectId = id;\n";
            for (const FieldSpec* field : privateOrder) {
                if (field->kind == FieldKind::Vec3) {
                    out << "    s_slots[n].state." << field->name << " = " << cVec3InitExpr(field->initializer) << ";\n";
                } else {
                    out << "    s_slots[n].state." << field->name << " = " << cScalarInitExpr(*field) << ";\n";
                }
            }
            out << "    return &s_slots[n].state;\n"
                << "}\n\n";
            // The runtime calls Script_ResetState on Stop and never unloads script modules, so
            // without this a C-lowered script's private fields would carry into the next Play
            // (the C++ backend resets through ModuCPP::ResetScriptState).
            out << "MODU_HOOK_ABI\nvoid Modu_ResetState(void) {\n"
                << "    int n;\n"
                << "    for (n = 0; n < MODUC_MAX_STATE_SLOTS; ++n) s_slots[n].used = 0;\n"
                << "}\n\n";
        }

        for (const LoweredMethod& helper : helpers) {
            out << "static " << helper.returnType << " " << helper.method->name
                << "(" << helper.params << ");\n";
        }
        if (!helpers.empty()) out << "\n";

        for (size_t idx = 0; idx < helpers.size(); ++idx) {
            out << "static " << helpers[idx].returnType << " " << helpers[idx].method->name
                << "(" << helpers[idx].params << ") {" << helperBodies[idx].text << "}\n\n";
        }

        auto cStringInit = [&](const FieldSpec& field) {
            const std::string init = trimCopy(field.initializer);
            return init.empty() ? std::string("\"\"") : init;
        };

        auto emitPublicLoad = [&](const FieldSpec& field, std::ostream& os) {
            if (field.kind == FieldKind::String || cFieldIsObjectRef(field)) {
                os << "    char " << field.name << "[MODUC_STRING_FIELD_MAX];\n"
                   << "    Modu_GetSettingString(ctx, \"" << field.name << "\", " << cStringInit(field)
                   << ", " << field.name << ", (int)sizeof(" << field.name << "));\n";
            } else if (field.kind == FieldKind::Bool) {
                os << "    const bool " << field.name << " = Modu_GetSettingBool(ctx, \""
                   << field.name << "\", " << cScalarInitExpr(field) << ") != 0;\n";
            } else if (field.kind == FieldKind::Int) {
                os << "    const int " << field.name << " = Modu_GetSettingInt(ctx, \""
                   << field.name << "\", " << cScalarInitExpr(field) << ");\n";
            } else if (field.kind == FieldKind::Vec3) {
                os << "    const ModuVec3 " << field.name << " = Modu_GetSettingVec3(ctx, \""
                   << field.name << "\", " << cVec3InitExpr(field.initializer) << ");\n";
            } else {
                os << "    const float " << field.name << " = Modu_GetSettingFloat(ctx, \""
                   << field.name << "\", " << cScalarInitExpr(field) << ");\n";
            }
        };

        for (size_t idx = 0; idx < lifecycle.size(); ++idx) {
            const LoweredMethod& hook = lifecycle[idx];
            const LoweredBody& body = lifecycleBodies[idx];
            const std::string dtName = hook.method->deltaTimeParamName.empty()
                ? "dt" : hook.method->deltaTimeParamName;
            out << "MODU_HOOK_ABI\nvoid " << hook.exportName << "(ModuScriptContext* ctx";
            if (hook.wantsDt) out << ", float " << dtName;
            out << ") {\n";
            if (body.usedState) {
                out << "    " << stateType << "* _moduState = " << san << "_GetState(ctx);\n";
            }
            for (const FieldSpec* field : publicOrder) {
                if (body.usedPublics.count(field->name)) emitPublicLoad(*field, out);
            }
            out << body.text << "\n}\n\n";
        }

        if (!publicOrder.empty()) {
            // Mirrors the C++ backend's emitAutoInspectorField widget for widget, so the
            // inspector looks the same whichever backend compiled the script.
            out << "MODU_HOOK_ABI\nvoid Modu_OnInspector(ModuScriptContext* ctx) {\n";
            for (const FieldSpec* field : publicOrder) {
                if (field->inspectorHeader) {
                    out << "    Modu_InspectorText(ctx, " << trimCopy(*field->inspectorHeader) << ");\n"
                        << "    Modu_InspectorSeparator(ctx);\n";
                }
                if (field->hasSeparatorAttribute) {
                    out << "    Modu_InspectorSeparator(ctx);\n";
                }
                const std::string key = field->name;
                const std::string label = escapeCStringLiteral(inspectorLabelFromFieldName(field->name));
                auto lit = [](const std::optional<std::string>& expr, const char* fallback) {
                    return expr ? trimCopy(*expr) : std::string(fallback);
                };
                out << "    {\n";
                if (cFieldIsObjectRef(*field)) {
                    out << "        char _moduV[MODUC_STRING_FIELD_MAX];\n"
                        << "        Modu_GetSettingString(ctx, \"" << key << "\", " << cStringInit(*field)
                        << ", _moduV, (int)sizeof(_moduV));\n"
                        << "        if (Modu_InspectorObjectRef(ctx, \"" << label << "\", _moduV, (int)sizeof(_moduV))) {\n"
                        << "            Modu_SetSettingString(ctx, \"" << key << "\", _moduV);\n"
                        << "        }\n";
                } else if (field->kind == FieldKind::String) {
                    out << "        char _moduV[MODUC_STRING_FIELD_MAX];\n"
                        << "        Modu_GetSettingString(ctx, \"" << key << "\", " << cStringInit(*field)
                        << ", _moduV, (int)sizeof(_moduV));\n"
                        << "        if (Modu_InspectorInputText(ctx, \"" << label << "\", _moduV, (int)sizeof(_moduV))) {\n"
                        << "            Modu_SetSettingString(ctx, \"" << key << "\", _moduV);\n"
                        << "        }\n";
                } else if (field->kind == FieldKind::Bool) {
                    out << "        int _moduV = Modu_GetSettingBool(ctx, \"" << key << "\", "
                        << cScalarInitExpr(*field) << ");\n"
                        << "        if (Modu_InspectorCheckbox(ctx, \"" << label << "\", &_moduV)) {\n"
                        << "            Modu_SetSettingBool(ctx, \"" << key << "\", _moduV);\n"
                        << "        }\n";
                } else if (field->kind == FieldKind::Int) {
                    out << "        int _moduV = Modu_GetSettingInt(ctx, \"" << key << "\", "
                        << cScalarInitExpr(*field) << ");\n";
                    std::string widget;
                    if (field->hasSliderAttribute) {
                        widget = "Modu_InspectorSliderInt(ctx, \"" + label + "\", &_moduV, (int)(" +
                                 trimCopy(*field->sliderMinExpr) + "), (int)(" + trimCopy(*field->sliderMaxExpr) + "))";
                    } else if (field->rangeMinExpr && field->rangeMaxExpr) {
                        widget = "Modu_InspectorSliderInt(ctx, \"" + label + "\", &_moduV, (int)(" +
                                 trimCopy(*field->rangeMinExpr) + "), (int)(" + trimCopy(*field->rangeMaxExpr) + "))";
                    } else {
                        widget = "Modu_InspectorInputInt(ctx, \"" + label + "\", &_moduV)";
                    }
                    out << "        if (" << widget << ") {\n"
                        << "            Modu_SetSettingInt(ctx, \"" << key << "\", _moduV);\n"
                        << "        }\n";
                } else if (field->kind == FieldKind::Vec3) {
                    out << "        ModuVec3 _moduV = Modu_GetSettingVec3(ctx, \"" << key << "\", "
                        << cVec3InitExpr(field->initializer) << ");\n";
                    std::string widget;
                    if (field->hasSliderAttribute) {
                        widget = "Modu_InspectorSliderFloat3(ctx, \"" + label + "\", &_moduV.x, " +
                                 trimCopy(*field->sliderMinExpr) + ", " + trimCopy(*field->sliderMaxExpr) + ", \"%.3f\")";
                    } else {
                        widget = "Modu_InspectorDragFloat3(ctx, \"" + label + "\", &_moduV.x, " +
                                 lit(field->stepExpr, "0.01f") + ", " + lit(field->rangeMinExpr, "-1000.0f") + ", " +
                                 lit(field->rangeMaxExpr, "1000.0f") + ", \"%.3f\")";
                    }
                    out << "        if (" << widget << ") {\n"
                        << "            Modu_SetSettingVec3(ctx, \"" << key << "\", _moduV);\n"
                        << "        }\n";
                } else {
                    out << "        float _moduV = Modu_GetSettingFloat(ctx, \"" << key << "\", "
                        << cScalarInitExpr(*field) << ");\n";
                    std::string widget;
                    if (field->hasSliderAttribute) {
                        widget = "Modu_InspectorSliderFloat(ctx, \"" + label + "\", &_moduV, " +
                                 trimCopy(*field->sliderMinExpr) + ", " + trimCopy(*field->sliderMaxExpr) + ")";
                    } else {
                        widget = "Modu_InspectorDragFloat(ctx, \"" + label + "\", &_moduV, " +
                                 lit(field->stepExpr, "0.01f") + ", " + lit(field->rangeMinExpr, "0.0f") + ", " +
                                 lit(field->rangeMaxExpr, "0.0f") + ", \"\")";
                    }
                    out << "        if (" << widget << ") {\n"
                        << "            Modu_SetSettingFloat(ctx, \"" << key << "\", _moduV);\n"
                        << "        }\n";
                }
                out << "    }\n";
            }
            out << "}\n";
        }

        outSource = out.str();
        reason.clear();
        return true;
    }
} // namespace

bool ModuCPPTranspiler::shouldTranspile(const fs::path& sourcePath, const std::string& sourceText) {
    std::string ext = toLowerCopy(sourcePath.extension().string());
    if (ext == ".moducpp") {
        return true;
    }

    // Allow high-level ModuCPP syntax in .cpp-like files without impacting normal C++ scripts.
    const std::string stripped = stripCommentsPreserveLayout(ModuCPPLang::CanonicalizeAuto(sourceText));
    return findModuClassDeclaration(stripped).has_value();
}

bool ModuCPPTranspiler::transpile(const fs::path& sourcePath, const std::string& sourceText, ModuCPPTranspileResult& outResult, std::string& error) const {
    error.clear();
    outResult = ModuCPPTranspileResult{};

    const std::string canonicalSource = ModuCPPLang::CanonicalizeAuto(sourceText);

    const std::string ext = toLowerCopy(sourcePath.extension().string());
    const std::string normalizedSource = normalizeModuSource(preprocessDollarStrings(unescapeAngleBracketTokens(canonicalSource)));
    const std::string stripped = stripCommentsPreserveLayout(normalizedSource);
    const bool hasHighLevelClass = findModuClassDeclaration(stripped).has_value();

    // Allow .moducpp as a thin frontend type for legacy/native C++ scripts during migration.
    // When high-level class syntax is absent, pass the source through unchanged.
    if (ext == ".moducpp" && !hasHighLevelClass) {
        outResult.generatedSource = normalizedSource;
        outResult.className = sourcePath.stem().string();
        return true;
    }

    ClassSpec parsed;
    if (!parseClass(normalizedSource, parsed, error)) {
        return false;
    }

    if (!cBackendDisabledByEnv()) {
        std::string cSource;
        std::string cReason;
        if (generateTranspiledCSource(sourcePath, parsed, cSource, cReason)) {
            outResult.generatedSource = std::move(cSource);
            outResult.className = parsed.name;
            outResult.generatedC = true;
            return true;
        }
        outResult.cBackendNote = cReason;
    }

    std::string generated = generateTranspiledSource(sourcePath, parsed, error);
    if (!error.empty()) {
        return false;
    }
    if (generated.empty()) {
        error = "ModuCPP transpiler generated empty output.";
        return false;
    }

    outResult.generatedSource = std::move(generated);
    outResult.className = parsed.name;
    return true;
}
