#pragma once
#include "ScriptSdkCommon.h"
#include <string>
#include <vector>
enum class Light2DType             {Point = 0,    Spot = 1,     Freeform = 2,    Sprite = 3,     Global = 4};
enum class Light2DBlendMode        {Additive = 0, Multiply = 1, Subtractive = 2};
enum class Light2DOverlapOperation {Additive = 0, Max = 1,      AlphaBlend = 2};
enum class Light2DNormalMapQuality {Disabled = 0, Fast = 1,     Accurate = 2};
struct Light2DBlendStyleDefinition {
    std::string name = "Additive";
    Light2DBlendMode mode = Light2DBlendMode::Additive;
    glm::vec4 modulate = glm::vec4(1.0f);
    float intensityScale = 1.0f;
};
struct Light2DFlickerSettings {bool enabled = false; float speed = 9.0f; float amount = 0.12f; float seed = 0.0f;};
struct Light2DComponent {
    bool enabled = true;
    Light2DType type = Light2DType::Point;
    glm::vec4 color = glm::vec4(1.0f);
    float intensity = 1.0f;
    float radius = 5.0f;
    float innerRadius = 0.0f;
    float outerRadius = 5.0f;
    float falloffStrength = 1.0f;
    float innerSpotAngle = 24.0f;
    float outerSpotAngle = 42.0f;
    int blendStyle = 0;
    int lightOrder = 0;
    Light2DOverlapOperation overlapOperation = Light2DOverlapOperation::Additive;
    float shadowStrength = 0.75f;
    bool volumetricEnabled = false;
    bool castsShadows = false;
    bool targetAllLayers = true;
    uint32_t targetLayerMask = 0xFFFFFFFFu;
    bool cullWhenOffscreen = true;
    float offscreenCullMargin = 0.0f;
    Light2DNormalMapQuality normalMapQuality = Light2DNormalMapQuality::Disabled;
    float normalMapDistance = 2.0f;
    bool useDistanceExponent = false;
    float distanceExponent = 1.0f;
    std::string cookieTexturePath;
    glm::vec2 cookieScale = glm::vec2(1.0f);
    float cookieRotation = 0.0f;
    float freeformFeather = 0.6f;
    float freeformEdgeFalloff = 1.0f;
    std::vector<glm::vec2> shapePoints;
    Light2DFlickerSettings flicker;
};
struct ShadowCaster2DComponent {
    bool enabled = true;
    bool castsSelfShadow = false;
    bool targetAllLayers = true;
    uint32_t targetLayerMask = 0xFFFFFFFFu;
    float shadowStrength = 1.0f;
    std::vector<glm::vec2> points;
};
