#include "SpritesheetFormat.h"
#include <cassert>
#include <cstdio>
#include <cctype>
namespace {
    enum class TokenType {
        Identifier,
        Number,
        String,
        LBrace,
        RBrace,
        Equals,
        Semicolon,
        Comma,
        End
    };
    struct Token {TokenType type = TokenType::End; std::string text; int line = 1;};
    struct Tokenizer {
        const std::string& input;
        size_t pos = 0;
        int line = 1;
        std::vector<SpritesheetParseMessage>* messages = nullptr;
        void warn(int warnLine, const std::string& text) {if (messages) messages->push_back({warnLine, true, text});}
        Token next() {
            while (pos < input.size()) {
                const char c = input[pos];
                if (c == ' ' || c == '\t' || c == '\r') {++pos; continue;}
                if (c == '\n')                          {++line; ++pos; continue;}
                if (c == '/' && pos + 1 < input.size() && input[pos + 1] == '/') {
                    pos += 2;
                    while (pos < input.size() && input[pos] != '\n') {++pos;}
                    continue;
                }
                Token token;
                token.line = line;
                switch (c) {
                    case '{': ++pos; token.type = TokenType::LBrace; token.text = "{"; return token;
                    case '}': ++pos; token.type = TokenType::RBrace; token.text = "}"; return token;
                    case '=': ++pos; token.type = TokenType::Equals; token.text = "="; return token;
                    case ';': ++pos; token.type = TokenType::Semicolon; token.text = ";"; return token;
                    case ',': ++pos; token.type = TokenType::Comma; token.text = ","; return token;
                    case '"': {
                        ++pos;
                        token.type = TokenType::String;
                        while (pos < input.size()) {
                            char ch = input[pos++];
                            if (ch == '\n') ++line;
                            if (ch == '"') return token;
                            if (ch == '\\' && pos < input.size()) {
                                char escaped = input[pos++];
                                if (escaped == 'n') token.text.push_back('\n');
                                else token.text.push_back(escaped);
                                continue;
                            }
                            token.text.push_back(ch);
                        }
                        warn(token.line, "Parse error at line " + std::to_string(token.line) + ": unterminated string literal");
                        return token;
                    }
                    default: break;
                }
                if (std::isdigit(static_cast<unsigned char>(c)) ||
                    ((c == '-' || c == '+') && pos + 1 < input.size() &&
                    (std::isdigit(static_cast<unsigned char>(input[pos + 1])) || input[pos + 1] == '.')) ||
                    (c == '.' && pos + 1 < input.size() && std::isdigit(static_cast<unsigned char>(input[pos + 1])))) {
                    token.type = TokenType::Number;
                    token.text.push_back(input[pos++]);
                    bool seenExponent = false;
                    while (pos < input.size()) {
                        const char next = input[pos];
                        if (std::isdigit(static_cast<unsigned char>(next)) || next == '.') {token.text.push_back(input[pos++]); continue;}
                        if (!seenExponent && (next == 'e' || next == 'E')) {
                            seenExponent = true;
                            token.text.push_back(input[pos++]);
                            if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) {token.text.push_back(input[pos++]);}
                            continue;
                        }
                        break;
                    }
                    return token;
                }
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                    token.type = TokenType::Identifier;
                    token.text.push_back(input[pos++]);
                    while (pos < input.size()) {
                        char ch = input[pos];
                        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.') {
                            token.text.push_back(ch); ++pos; continue;
                        }
                        break;
                    }
                    return token;
                }
                warn(line, "Parse error at line " + std::to_string(line) + ": unexpected character '" + std::string(1, c) + "'");
                ++pos;
            }
            return Token{TokenType::End, "", line};
        }
    };
    struct Parser {
        std::vector<Token> tokens;
        size_t index = 0;
        SpritesheetParseResult result;
        const Token& peek(size_t offset = 0) const {const size_t i = std::min(index + offset, tokens.size() - 1); return tokens[i];}
        const Token& advance() {
            const size_t i = std::min(index, tokens.size() - 1);
            if (index < tokens.size() - 1) ++index;
            return tokens[i];
        }
        bool match(TokenType type) { if (peek().type != type) return false; advance(); return true;}
        void error(int line, const std::string& text) {
            result.messages.push_back({line, true, "Parse error at line " + std::to_string(line) + ": " + text});
        }
        void syncToTopLevel() {
            int depth = 0;
            while (peek().type != TokenType::End) {
                if (peek().type == TokenType::LBrace) ++depth;
                else if (peek().type == TokenType::RBrace) {if (depth == 0) return; --depth;}
                else if (depth == 0 && (peek().type == TokenType::Identifier || peek().type == TokenType::RBrace)) return;
                advance();
            }
        }
        bool parseInt(int& out) {
            if (peek().type != TokenType::Number) return false;
            out = std::stoi(advance().text);  return true;
        }
        bool parseFloat(float& out) {
            if (peek().type != TokenType::Number) return false;
            out = std::stof(advance().text);  return true;
        }
        bool parseAssignmentValue(const Token& key) {
            if (!match(TokenType::Equals)) {
                error(key.line, "expected '=' after identifier '" + key.text + "'");
                if (peek().type == TokenType::Equals) advance();
                syncToTopLevel(); return false;
            }
            if (key.text == "LinkedSpriteName" ||
                key.text == "LastSavedUtc" ||
                key.text == "ExpectedMinimumModuEngineVersionOrHigher") {
                if (peek().type != TokenType::String) {
                    error(peek().line, "expected string value for '" + key.text + "'");
                    syncToTopLevel(); return false;
                }
                const std::string value = advance().text;
                match(TokenType::Semicolon);
                if (key.text == "LinkedSpriteName") result.document.linkedSpriteName = value;
                else if (key.text == "LastSavedUtc") result.document.lastSavedUtc = value;
                else result.document.expectedMinimumModuEngineVersionOrHigher = value;
                return true;
            }
            if (key.text == "SpriteVersion" || key.text == "Expect_Layers" || key.text == "Expect_rects") {
                int value = 0;
                if (!parseInt(value)) {
                    error(peek().line, "expected integer value for '" + key.text + "'");
                    syncToTopLevel(); return false;
                }
                match(TokenType::Semicolon);
                if (key.text == "SpriteVersion") result.document.spriteVersion = value;
                else if (key.text == "Expect_Layers") result.document.expectLayers = value;
                else result.document.expectRects = value;
                return true;
            }
            if (key.text == "Confirmation.StrictValidation") {
                if (peek().type != TokenType::Identifier || (peek().text != "true" && peek().text != "false")) {
                    error(peek().line, "expected boolean value for '" + key.text + "'");
                    syncToTopLevel(); return false;
                }
                result.document.strictValidation = (advance().text == "true");
                match(TokenType::Semicolon);
                return true;
            }
            error(key.line, "unknown assignment '" + key.text + "'");
            syncToTopLevel();
            return false;
        }
        void skipUnknownBlock() {
            if (!match(TokenType::LBrace)) return;
            int depth = 1;
            while (peek().type != TokenType::End && depth > 0) {
                if (match(TokenType::LBrace)) ++depth;
                else if (match(TokenType::RBrace)) --depth;
                else advance();
            }
        }
        void parseRectsBlock() {
            std::vector<glm::ivec4> parsedRects;
            if (!match(TokenType::LBrace)) {error(peek().line, "expected '{' after rects"); return;}

            while (peek().type != TokenType::End && peek().type != TokenType::RBrace) {
                glm::ivec4 rect(0);
                const int entryLine = peek().line;
                bool ok = true;
                for (int i = 0; i < 4; ++i) {
                    if (!parseInt(rect[i])) {
                        error(peek().line, "unexpected token '" + peek().text + "' inside rects block");
                        ok = false; break;
                    }
                    if (i < 3 && !match(TokenType::Comma)) {
                        error(peek().line, "expected ',' inside rects block");
                        ok = false; break;
                    }
                }
                if (ok) {
                    if (!match(TokenType::Semicolon)) {
                        error(peek().line, "expected ';' after rect entry");
                    }
                    rect.z = std::max(1, rect.z);
                    rect.w = std::max(1, rect.w);
                    parsedRects.push_back(rect);
                } else {
                    while (peek().type != TokenType::End && peek().type != TokenType::Semicolon && peek().type != TokenType::RBrace) advance();
                    match(TokenType::Semicolon);
                    parsedRects.clear();
                    while (peek().type != TokenType::End && peek().line == entryLine && peek().type != TokenType::RBrace) advance();
                }
            }
            match(TokenType::RBrace);
            result.document.rects = parsedRects;
        }
        void parseNamesBlock() {
            std::vector<std::string> parsedNames;
            if (!match(TokenType::LBrace)) {error(peek().line, "expected '{' after names"); return;}
            while (peek().type != TokenType::End && peek().type != TokenType::RBrace) {
                if (match(TokenType::Semicolon)) {parsedNames.emplace_back(); continue;}
                if (peek().type == TokenType::Identifier) {
                    parsedNames.push_back(advance().text);
                    if (!match(TokenType::Semicolon)) {error(peek().line, "expected ';' after name entry");}
                    continue;
                }
                error(peek().line, "unexpected token '" + peek().text + "' inside names block");
                while (peek().type != TokenType::End && peek().type != TokenType::Semicolon && peek().type != TokenType::RBrace) advance();
                match(TokenType::Semicolon);
                parsedNames.clear();
            }
            match(TokenType::RBrace);
            result.document.names = parsedNames;
        }
        void parseScalesBlock() {
            std::vector<glm::vec2> parsedScales;
            if (!match(TokenType::LBrace)) {error(peek().line, "expected '{' after scales");return;}
            while (peek().type != TokenType::End && peek().type != TokenType::RBrace) {
                glm::vec2 scale(1.0f);
                const int entryLine = peek().line;
                bool ok = true;
                if (!parseFloat(scale.x)) {error(peek().line, "unexpected token '" + peek().text + "' inside scales block");      ok = false;}
                else if (!match(TokenType::Comma)) { error(peek().line, "expected ',' inside scales block");                      ok = false;}
                else if (!parseFloat(scale.y)) {error(peek().line, "unexpected token '" + peek().text + "' inside scales block"); ok = false;}
                if (ok) {
                    if (!match(TokenType::Semicolon)) {error(peek().line, "expected ';' after scale entry");}
                    scale.x = std::max(0.01f, scale.x);
                    scale.y = std::max(0.01f, scale.y);
                    parsedScales.push_back(scale);
                } else {
                    while (peek().type != TokenType::End && peek().type != TokenType::Semicolon && peek().type != TokenType::RBrace) advance();
                    match(TokenType::Semicolon);
                    parsedScales.clear();
                    while (peek().type != TokenType::End && peek().line == entryLine && peek().type != TokenType::RBrace) advance();
                }
            }
            match(TokenType::RBrace);
            result.document.scales = parsedScales;
        }
        void parseLayersBlock() {
            if (!match(TokenType::LBrace)) {error(peek().line, "expected '{' after info Layers"); return;}
            std::vector<SpritesheetLayer> layers;
            while (peek().type != TokenType::End && peek().type != TokenType::RBrace) {
                if (peek().type == TokenType::Identifier && peek().text == "names") {
                    advance();
                    if (!match(TokenType::LBrace)) {error(peek().line, "expected '{' after layer names"); continue;}
                    while (peek().type != TokenType::End && peek().type != TokenType::RBrace) {
                        if (match(TokenType::Semicolon)) {
                            layers.push_back({ "Layer_" + std::to_string(layers.size()), true, "" }); continue;
                        }
                        if (peek().type == TokenType::Identifier || peek().type == TokenType::String) {
                            layers.push_back({ advance().text, true, "" });
                            match(TokenType::Semicolon); continue;
                        }
                        advance();
                    }
                    match(TokenType::RBrace);
                    continue;
                }
                if (peek().type == TokenType::Identifier && peek().text == "visible") {
                    advance();
                    if (!match(TokenType::LBrace)) {error(peek().line, "expected '{' after layer visibility"); continue;}
                    size_t visibleIndex = 0;
                    while (peek().type != TokenType::End && peek().type != TokenType::RBrace) {
                        if (peek().type == TokenType::Identifier && (peek().text == "true" || peek().text == "false")) {
                            if (visibleIndex >= layers.size()) {
                                layers.resize(visibleIndex + 1);
                                if (layers[visibleIndex].name.empty()) {layers[visibleIndex].name = "Layer_" + std::to_string(visibleIndex);}
                            }
                            layers[visibleIndex].visible = (advance().text == "true");
                            ++visibleIndex;
                            match(TokenType::Semicolon);
                            continue;
                        }
                        if (match(TokenType::Semicolon)) {++visibleIndex; continue;}
                        advance();
                    }
                    match(TokenType::RBrace);
                    continue;
                }
                if (peek().type == TokenType::Identifier && peek().text == "pixels") {
                    advance();
                    if (!match(TokenType::LBrace)) {error(peek().line, "expected '{' after layer pixels"); continue;}
                    size_t pixelIndex = 0;
                    while (peek().type != TokenType::End && peek().type != TokenType::RBrace) {
                        if (peek().type == TokenType::String) {
                            if (pixelIndex >= layers.size()) {
                                layers.resize(pixelIndex + 1);
                                if (layers[pixelIndex].name.empty()) {layers[pixelIndex].name = "Layer_" + std::to_string(pixelIndex);}
                            }
                            layers[pixelIndex].pixelData = advance().text;
                            ++pixelIndex;
                            match(TokenType::Semicolon);
                            continue;
                        }
                        if (match(TokenType::Semicolon)) {++pixelIndex; continue;}
                        advance();
                    }
                    match(TokenType::RBrace);
                    continue;
                }
                if (peek().type == TokenType::Identifier) {
                    advance();
                    if (peek().type == TokenType::LBrace) skipUnknownBlock();
                    else {
                        while (peek().type != TokenType::End && peek().type != TokenType::RBrace && peek().type != TokenType::Semicolon) advance();
                        match(TokenType::Semicolon);
                    }
                    continue;
                }
                advance();
            }
            match(TokenType::RBrace);
            result.document.layers = layers;
        }
        void parseAtlasInfoBlock() {
            std::vector<glm::ivec4> defaultRects = result.document.rects;
            std::vector<std::string> defaultNames = result.document.names;
            std::vector<glm::vec2> defaultScales = result.document.scales;
            if (!match(TokenType::LBrace)) {error(peek().line, "expected '{' after info AtlasInfo"); return;}
            while (peek().type != TokenType::End && peek().type != TokenType::RBrace) {
                if (peek().type != TokenType::Identifier) {
                    error(peek().line, "unexpected token '" + peek().text + "' inside info AtlasInfo");
                    advance(); continue;
                }
                const std::string blockName = advance().text;
                if (blockName == "rects") parseRectsBlock();
                else if (blockName == "names") parseNamesBlock();
                else if (blockName == "scales") parseScalesBlock();
                else {
                    error(peek().line, "unexpected block '" + blockName + "' inside info AtlasInfo");
                    if (peek().type == TokenType::LBrace) skipUnknownBlock();
                }
            }
            match(TokenType::RBrace);
            if (result.document.rects.empty() && !defaultRects.empty()) result.document.rects = defaultRects;
            if (result.document.names.empty() && !defaultNames.empty()) result.document.names = defaultNames;
            if (result.document.scales.empty() && !defaultScales.empty()) result.document.scales = defaultScales;
        }
        void parseInfoBlock() {
            const Token infoToken = advance();
            if (peek().type != TokenType::Identifier) {error(peek().line, "expected identifier after info"); return;}
            const Token blockName = advance();
            if (blockName.text == "AtlasInfo") parseAtlasInfoBlock();
            else if (blockName.text == "Layers") parseLayersBlock();
            else {
                error(blockName.line, "unknown info block '" + blockName.text + "'");
                if (peek().type == TokenType::LBrace) skipUnknownBlock();
            }
            (void)infoToken;
        }
        void finalize() {
            if (result.document.expectRects <= 0)                                  {result.document.expectRects = static_cast<int>(result.document.rects.size());}
            if (result.document.expectLayers <= 0)                                 {result.document.expectLayers = std::max(1, static_cast<int>(result.document.layers.size()));}
            if (result.document.names.size() < result.document.rects.size())       {result.document.names.resize(result.document.rects.size());}
            else if (result.document.names.size() > result.document.rects.size())  {result.document.names.resize(result.document.rects.size());}
            if (result.document.scales.size() < result.document.rects.size())      {result.document.scales.resize(result.document.rects.size(), glm::vec2(1.0f));}
            else if (result.document.scales.size() > result.document.rects.size()) {result.document.scales.resize(result.document.rects.size());}
            for (size_t i = 0; i < result.document.names.size(); ++i) {
                if (result.document.names[i].empty()) {result.document.names[i] = "Rect_" + std::to_string(i);}
            }
            for (glm::vec2& scale : result.document.scales) {
                scale.x = std::max(0.01f, scale.x);
                scale.y = std::max(0.01f, scale.y);
            }
            for (size_t i = 0; i < result.document.layers.size(); ++i) {
                if (result.document.layers[i].name.empty()) {result.document.layers[i].name = "Layer_" + std::to_string(i);}
            }
        }
        SpritesheetParseResult parse() {
            while (peek().type != TokenType::End) {
                if (peek().type != TokenType::Identifier) {
                    error(peek().line, "unexpected token '" + peek().text + "' at top level");
                    advance(); continue;
                }
                if (peek().text == "info") {parseInfoBlock(); continue;}
                const Token key = advance();
                parseAssignmentValue(key);
            }
            finalize();
            return result;
        }
    };
    std::string EscapeString(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            if (c == '\\' || c == '"') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }
    std::string CurrentUtcIso8601() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
        std::tm tmUtc{};
    #ifdef _WIN32
        gmtime_s(&tmUtc, &timeValue);
    #else
        gmtime_r(&timeValue, &tmUtc);
    #endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
        return buffer;
    }
    #ifndef NDEBUG
    bool ContainsMessage(const std::vector<SpritesheetParseMessage>& messages, const std::string& needle) {
        for (const auto& message : messages) {
            if (message.text.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    #endif
}
SpritesheetParseResult ParseSpritesheet(const std::string& text) {
    static bool selfTestsRan = false;
    if (!selfTestsRan) {selfTestsRan = true; RunSpritesheetParserSelfTests();}
    SpritesheetParseResult result;
    Tokenizer tokenizer{text, 0, 1, &result.messages};
    std::vector<Token> tokens;
    while (true) {
        Token token = tokenizer.next();
        tokens.push_back(token);
        if (token.type == TokenType::End) break;
    }
    Parser parser;
    parser.tokens = std::move(tokens);
    parser.result = std::move(result);
    return parser.parse();
}
std::string WriteSpritesheet(const SpritesheetDocument& inputDocument) {
    SpritesheetDocument document = inputDocument;
    if (document.spriteVersion <= 0) document.spriteVersion = 1;
    if (document.expectLayers <= 0) document.expectLayers = std::max(1, static_cast<int>(document.layers.size()));
    if (document.expectRects <= 0) document.expectRects = static_cast<int>(document.rects.size());
    if (document.lastSavedUtc.empty()) document.lastSavedUtc = CurrentUtcIso8601();
    if (document.names.size() < document.rects.size()) document.names.resize(document.rects.size());
    if (document.names.size() > document.rects.size()) document.names.resize(document.rects.size());
    if (document.scales.size() < document.rects.size()) document.scales.resize(document.rects.size(), glm::vec2(1.0f));
    if (document.scales.size() > document.rects.size()) document.scales.resize(document.rects.size());
    for (size_t i = 0; i < document.names.size(); ++i) {
        if (document.names[i].empty()) document.names[i] = "Rect_" + std::to_string(i);
    }
    for (glm::vec2& scale : document.scales) {scale.x = std::max(0.01f, scale.x); scale.y = std::max(0.01f, scale.y);}
    for (size_t i = 0; i < document.layers.size(); ++i) {
        if (document.layers[i].name.empty()) document.layers[i].name = "Layer_" + std::to_string(i);
    }
    std::ostringstream out;
    out << "LinkedSpriteName = \"" << EscapeString(document.linkedSpriteName) << "\";\n";
    out << "SpriteVersion = " << document.spriteVersion << ";\n";
    out << "LastSavedUtc = \"" << EscapeString(document.lastSavedUtc) << "\"; // Don't edit this, it updates automatically when you save this script, don't worry!\n\n";
    out << "ExpectedMinimumModuEngineVersionOrHigher = \"" << EscapeString(document.expectedMinimumModuEngineVersionOrHigher) << "\";\n";
    out << "Expect_Layers = " << document.expectLayers << ";\n";
    out << "Expect_rects = " << document.expectRects << ";\n\n";
    out << "Confirmation.StrictValidation = " << (document.strictValidation ? "true" : "false") << ";\n";
    out << "// This above is a toggle switch for stricter values, enable this if you really want info if something is wrong.\n\n";
    out << "// this stores info of the atlas sprites and the rects below, you can edit them to crop positions, (or just do it in the Spritesheet editor lol.)\n";
    out << "info AtlasInfo\n";
    out << "{\n";
    out << "    rects\n";
    out << "    {\n";
    out << "        // To edit this: remember the position values (x, y, w, h).\n";
    for (const glm::ivec4& rect : document.rects) {out << "        " << rect.x << "," << rect.y << "," << rect.z << "," << rect.w << ";\n";}
    out << "    }\n\n";
    out << "    names\n";
    out << "    {\n";
    out << "        // To edit this: Each name corresponds to the names of the atlas position values above, Leave it empty to name it by ints or name it by name below.\n";
    for (const std::string& name : document.names) {out << "        " << name << ";\n";}
    out << "    }\n";
    out << "\n";
    out << "    scales\n";
    out << "    {\n";
    out << "        // Per-clip display multipliers. Leave these at 1,1 to use the clip rect size ratio automatically.\n";
    for (const glm::vec2& scale : document.scales) {out << "        " << scale.x << "," << scale.y << ";\n";}
    out << "    }\n";
    out << "}\n\n";
    out << "info Layers\n";
    out << "{\n";
    out << "    // If you haven't used layers, ignore placing them here, it's best to let the Edit Mode handle Layering as it's pretty strict, unless you want hell of course.\n";
    if (!document.layers.empty()) {
        out << "\n";
        out << "    names\n";
        out << "    {\n";
        for (const SpritesheetLayer& layer : document.layers) {out << "        \"" << EscapeString(layer.name) << "\";\n";}
        out << "    }\n";
        out << "\n";
        out << "    visible\n";
        out << "    {\n";
        for (const SpritesheetLayer& layer : document.layers) {out << "        " << (layer.visible ? "true" : "false") << ";\n";}
        out << "    }\n";
        out << "\n";
        out << "    pixels\n";
        out << "    {\n";
        for (const SpritesheetLayer& layer : document.layers) {out << "        \"" << EscapeString(layer.pixelData) << "\";\n";}
        out << "    }\n";
    }
    out << "}\n";
    return out.str();
}
void RunSpritesheetParserSelfTests() {
#ifndef NDEBUG
    const std::string valid =
        "LinkedSpriteName = \"Assets/Sprites/sprite.png\";\n"
        "SpriteVersion = 1;\n"
        "LastSavedUtc = \"2026-03-02T06:09:00Z\"; // note\n"
        "ExpectedMinimumModuEngineVersionOrHigher = \"ModuEngine V6.7\";\n"
        "Expect_Layers = 1;\n"
        "Expect_rects = 2;\n"
        "Confirmation.StrictValidation = false;\n"
        "info AtlasInfo { rects { 1,2,3,4; 5,6,7,8; } names { A; ; } scales { 1,1; 1.5,0.5; } }\n"
        "info Layers { }\n";
    const auto validResult = ParseSpritesheet(valid);
    assert(validResult.document.rects.size() == 2);
    assert(validResult.document.names.size() == 2);
    assert(validResult.document.names[1] == "Rect_1");
    assert(validResult.document.scales.size() == 2);
    assert(std::abs(validResult.document.scales[1].x - 1.5f) < 0.001f);
    const std::string whitespace =
        "LinkedSpriteName= \"A\" ;\n"
        "SpriteVersion =1\n"
        "ExpectedMinimumModuEngineVersionOrHigher = \"B\";\n"
        "Expect_Layers= 1 ;\n"
        "Expect_rects =2;\n"
        "Confirmation.StrictValidation = false\n"
        "info AtlasInfo{rects{1,1,1,1;2,2,2,2;}names{X;Y;}scales{1,1;0.75,1.25;}}\n"
        "info Layers{}\n";
    const auto whitespaceResult = ParseSpritesheet(whitespace);
    assert(whitespaceResult.document.linkedSpriteName == "A");
    assert(whitespaceResult.document.rects.size() == 2);
    assert(std::abs(whitespaceResult.document.scales[1].y - 1.25f) < 0.001f);
    const std::string invalid = "LinkedSpriteName \"bad\";\n" "SpriteVersion == 2;\n" "info AtlasInfo { rects { 1,2,3; ; } names { A; } scales { nope; } }\n";
    const auto invalidResult = ParseSpritesheet(invalid);
    assert(invalidResult.document.linkedSpriteName.empty());
    assert(invalidResult.document.spriteVersion == 1);
    assert(ContainsMessage(invalidResult.messages, "expected '=' after identifier 'LinkedSpriteName'"));
    assert(ContainsMessage(invalidResult.messages, "expected integer value for 'SpriteVersion'") || ContainsMessage(invalidResult.messages, "unexpected token '='"));
#endif
}
