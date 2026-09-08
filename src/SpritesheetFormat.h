#pragma once
#include "Common.h"
struct SpritesheetLayer {std::string name; bool visible = true; std::string pixelData;};
struct SpritesheetDocument {
    std::string linkedSpriteName;
    int spriteVersion = 1;
    std::string lastSavedUtc;
    std::string expectedMinimumModuEngineVersionOrHigher;
    int expectLayers = 1;
    int expectRects = 0;
    bool strictValidation = false;
    std::vector<glm::ivec4> rects;
    std::vector<std::string> names;
    std::vector<glm::vec2> scales;
    std::vector<SpritesheetLayer> layers;
};
struct SpritesheetParseMessage {int line = 1; bool error = true; std::string text;};
struct SpritesheetParseResult  {SpritesheetDocument document;    std::vector<SpritesheetParseMessage> messages;};
SpritesheetParseResult ParseSpritesheet(const std::string& text);
std::string WriteSpritesheet(const SpritesheetDocument& document);
void RunSpritesheetParserSelfTests();
