#pragma once

// ModuUIScriptApi.h
// obj.UI scripting API for ModuCPP - provides safe, LSP-friendly access to UI components.
// Included automatically via ModuCPPScriptApi.h when "add ModuCPP;" is used.

#include "ScriptRuntime.h"
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ModuCPP {

// Color helper

inline glm::vec4 Color(float r, float g, float b, float a = 1.0f) {
    return glm::vec4(r, g, b, a);
}

inline glm::vec4 Color(float rgb, float a = 1.0f) {
    return glm::vec4(rgb, rgb, rgb, a);
}

// Internal storage for UI events and state

namespace detail {

struct UICallbacks {
    std::function<void()>      onClick;
    std::function<void()>      onHover;
    std::function<void()>      onUnhover;
    std::function<void()>      onPress;
    std::function<void()>      onRelease;
    std::function<void()>      onUnclick;
    std::function<void(float)> onValueChanged;   // slider
    std::function<void(bool)>  onToggleChanged;  // toggle
    std::function<void(const std::string&)> onInputSubmit; // input
};

struct UIEventState {
    bool  wasHovered = false;
    bool  wasActive  = false;
    bool  wasClicked = false;
    float lastSliderValue = -1e38f; // sentinel: uninitialized
};

inline std::unordered_map<std::string, UICallbacks>& uiCallbackStore() {
    static std::unordered_map<std::string, UICallbacks> s;
    return s;
}

inline std::unordered_map<std::string, UIEventState>& uiEventStateStore() {
    static std::unordered_map<std::string, UIEventState> s;
    return s;
}

// Warning-once: keyed by "objectName|feature"
inline std::unordered_set<std::string>& uiWarnedSet() {
    static std::unordered_set<std::string> s;
    return s;
}

inline void uiWarnOnce(ScriptContext* ctx, const char* feature) {
    if (!ctx || !ctx->object) return;
    const std::string key = ctx->object->name + "|" + feature;
    if (uiWarnedSet().count(key)) return;
    uiWarnedSet().insert(key);
    if (!ctx->object->hasUI) {
        ctx->AddConsoleMessage(
            "[UI] '" + ctx->object->name + "' has no UI component — cannot access '" +
            std::string(feature) + "'.", ConsoleMessageType::Warning);
    } else {
        ctx->AddConsoleMessage(
            "[UI] '" + ctx->object->name + "' UI type does not support '" +
            std::string(feature) + "'.", ConsoleMessageType::Warning);
    }
}

} // namespace detail

// Forward declarations

struct UIProxy;

// UIButtonProxy  -  obj.UI.Button.*

struct UIButtonProxy {
    ScriptContext* ctx_;

    explicit UIButtonProxy(ScriptContext* ctx) : ctx_(ctx) {}

    bool hasButton() const {
        return ctx_ && ctx_->object && ctx_->object->hasUI &&
               ctx_->object->ui.type == UIElementType::Button;
    }

    // Text / Label
    UIButtonProxy& operator=(const std::string&) = delete;

    struct TextProp {
        UIButtonProxy* p;
        TextProp& operator=(const std::string& v) {
            if (p->hasButton()) p->ctx_->object->ui.label = v;
            else detail::uiWarnOnce(p->ctx_, "Button.Text");
            return *this;
        }
        TextProp& operator=(const char* v) { return *this = std::string(v ? v : ""); }
        operator std::string() const {
            return p->hasButton() ? p->ctx_->object->ui.label : std::string{};
        }
    };
    TextProp Text{ this };

    struct EnabledProp {
        UIButtonProxy* p;
        EnabledProp& operator=(bool v) {
            if (p->hasButton()) p->ctx_->object->ui.interactable = v;
            else detail::uiWarnOnce(p->ctx_, "Button.Enabled");
            return *this;
        }
        operator bool() const {
            return p->hasButton() && p->ctx_->object->ui.interactable;
        }
    };
    EnabledProp Enabled{ this };

    struct InteractableProp {
        UIButtonProxy* p;
        InteractableProp& operator=(bool v) {
            if (p->hasButton()) p->ctx_->object->ui.interactable = v;
            else detail::uiWarnOnce(p->ctx_, "Button.Interactable");
            return *this;
        }
        operator bool() const {
            return p->hasButton() && p->ctx_->object->ui.interactable;
        }
    };
    InteractableProp Interactable{ this };
};

// UISliderProxy  -  obj.UI.Slider.*

struct UISliderProxy {
    ScriptContext* ctx_;
    const std::string* instanceKey_;

    UISliderProxy(ScriptContext* ctx, const std::string* key)
        : ctx_(ctx), instanceKey_(key) {}

    bool hasSlider() const {
        return ctx_ && ctx_->object && ctx_->object->hasUI &&
               ctx_->object->ui.type == UIElementType::Slider;
    }

    struct LabelProp {
        UISliderProxy* p;
        LabelProp& operator=(const std::string& v) {
            if (p->hasSlider()) p->ctx_->object->ui.label = v;
            else detail::uiWarnOnce(p->ctx_, "Slider.Label");
            return *this;
        }
        LabelProp& operator=(const char* v) { return *this = std::string(v ? v : ""); }
        operator std::string() const {
            return p->hasSlider() ? p->ctx_->object->ui.label : std::string{};
        }
    };
    LabelProp Label{ this };

    struct ValueProp {
        UISliderProxy* p;
        ValueProp& operator=(float v) {
            if (p->hasSlider()) {
                p->ctx_->object->ui.sliderValue = std::clamp(v,
                    p->ctx_->object->ui.sliderMin,
                    p->ctx_->object->ui.sliderMax);
            } else {
                detail::uiWarnOnce(p->ctx_, "Slider.Value");
            }
            return *this;
        }
        operator float() const {
            return p->hasSlider() ? p->ctx_->object->ui.sliderValue : 0.0f;
        }
    };
    ValueProp Value{ this };

    struct MinProp {
        UISliderProxy* p;
        MinProp& operator=(float v) {
            if (p->hasSlider()) p->ctx_->object->ui.sliderMin = v;
            else detail::uiWarnOnce(p->ctx_, "Slider.Min");
            return *this;
        }
        operator float() const {
            return p->hasSlider() ? p->ctx_->object->ui.sliderMin : 0.0f;
        }
    };
    MinProp Min{ this };

    struct MaxProp {
        UISliderProxy* p;
        MaxProp& operator=(float v) {
            if (p->hasSlider()) p->ctx_->object->ui.sliderMax = v;
            else detail::uiWarnOnce(p->ctx_, "Slider.Max");
            return *this;
        }
        operator float() const {
            return p->hasSlider() ? p->ctx_->object->ui.sliderMax : 1.0f;
        }
    };
    MaxProp Max{ this };

    struct FillColorProp {
        UISliderProxy* p;
        FillColorProp& operator=(const glm::vec4& v) {
            if (p->hasSlider()) p->ctx_->object->ui.fillColor = v;
            else detail::uiWarnOnce(p->ctx_, "Slider.FillColor");
            return *this;
        }
        operator glm::vec4() const {
            return p->hasSlider() ? p->ctx_->object->ui.fillColor : glm::vec4(0.0f);
        }
    };
    FillColorProp FillColor{ this };

    struct BackgroundColorProp {
        UISliderProxy* p;
        BackgroundColorProp& operator=(const glm::vec4& v) {
            if (p->hasSlider()) p->ctx_->object->ui.backgroundColor = v;
            else detail::uiWarnOnce(p->ctx_, "Slider.BackgroundColor");
            return *this;
        }
        operator glm::vec4() const {
            return p->hasSlider() ? p->ctx_->object->ui.backgroundColor : glm::vec4(0.0f);
        }
    };
    BackgroundColorProp BackgroundColor{ this };

    struct TextColorProp {
        UISliderProxy* p;
        TextColorProp& operator=(const glm::vec4& v) {
            if (p->hasSlider()) p->ctx_->object->ui.textColor = v;
            else detail::uiWarnOnce(p->ctx_, "Slider.TextColor");
            return *this;
        }
        operator glm::vec4() const {
            return p->hasSlider() ? p->ctx_->object->ui.textColor : glm::vec4(0.0f);
        }
    };
    TextColorProp TextColor{ this };

    struct TextSizeProp {
        UISliderProxy* p;
        TextSizeProp& operator=(float v) {
            if (p->hasSlider()) p->ctx_->object->ui.fontSize = std::max(0.0f, v);
            else detail::uiWarnOnce(p->ctx_, "Slider.TextSize");
            return *this;
        }
        operator float() const {
            return p->hasSlider() ? p->ctx_->object->ui.fontSize : 0.0f;
        }
    };
    TextSizeProp TextSize{ this };

    void OnValueChanged(std::function<void(float)> fn) {
        if (!instanceKey_) return;
        detail::uiCallbackStore()[*instanceKey_].onValueChanged = std::move(fn);
    }
};

// UIImageProxy  -  obj.UI.Image.*

struct UIImageProxy {
    ScriptContext* ctx_;

    explicit UIImageProxy(ScriptContext* ctx) : ctx_(ctx) {}

    bool hasImage() const {
        return ctx_ && ctx_->object && ctx_->object->hasUI &&
               (ctx_->object->ui.type == UIElementType::Image ||
                ctx_->object->ui.type == UIElementType::Sprite2D);
    }

    struct TextureProp {
        UIImageProxy* p;
        TextureProp& operator=(const std::string& v) {
            if (p->hasImage()) p->ctx_->object->albedoTexturePath = v;
            else detail::uiWarnOnce(p->ctx_, "Image.Texture");
            return *this;
        }
        TextureProp& operator=(const char* v) { return *this = std::string(v ? v : ""); }
        operator std::string() const {
            return (p->hasImage() && p->ctx_->object) ? p->ctx_->object->albedoTexturePath : std::string{};
        }
    };
    TextureProp Texture{ this };

    struct TintProp {
        UIImageProxy* p;
        TintProp& operator=(const glm::vec4& v) {
            if (p->hasImage()) p->ctx_->object->ui.color = v;
            else detail::uiWarnOnce(p->ctx_, "Image.Tint");
            return *this;
        }
        operator glm::vec4() const {
            return p->hasImage() ? p->ctx_->object->ui.color : glm::vec4(1.0f);
        }
    };
    TintProp Tint{ this };
};

// UIToggleProxy  -  obj.UI.Toggle.* (stubbed - no engine type yet)

struct UIToggleProxy {
    ScriptContext* ctx_;
    const std::string* instanceKey_;

    UIToggleProxy(ScriptContext* ctx, const std::string* key)
        : ctx_(ctx), instanceKey_(key) {}

    struct ValueProp {
        UIToggleProxy* p;
        ValueProp& operator=(bool) {
            detail::uiWarnOnce(p->ctx_, "Toggle.Value");
            return *this;
        }
        operator bool() const { return false; }
    };
    ValueProp Value{ this };

    void OnChanged(std::function<void(bool)> fn) {
        if (!instanceKey_) return;
        detail::uiCallbackStore()[*instanceKey_].onToggleChanged = std::move(fn);
    }
};

// UIInputProxy  -  obj.UI.Input.* (stubbed - no engine type yet)

struct UIInputProxy {
    ScriptContext* ctx_;
    const std::string* instanceKey_;

    UIInputProxy(ScriptContext* ctx, const std::string* key)
        : ctx_(ctx), instanceKey_(key) {}

    struct TextProp {
        UIInputProxy* p;
        TextProp& operator=(const std::string&) {
            detail::uiWarnOnce(p->ctx_, "Input.Text");
            return *this;
        }
        TextProp& operator=(const char* v) { return *this = std::string(v ? v : ""); }
        operator std::string() const { return {}; }
    };
    TextProp Text{ this };

    struct PlaceholderProp {
        UIInputProxy* p;
        PlaceholderProp& operator=(const std::string&) {
            detail::uiWarnOnce(p->ctx_, "Input.Placeholder");
            return *this;
        }
        PlaceholderProp& operator=(const char* v) { return *this = std::string(v ? v : ""); }
        operator std::string() const { return {}; }
    };
    PlaceholderProp Placeholder{ this };

    void OnSubmit(std::function<void(const std::string&)> fn) {
        if (!instanceKey_) return;
        detail::uiCallbackStore()[*instanceKey_].onInputSubmit = std::move(fn);
    }
};

// UIProxy  -  obj.UI

struct UIProxy {
    ScriptContext* ctx_   = nullptr;
    std::string instanceKey_;

    // Sub-proxies (initialized in constructor)
    UIButtonProxy  Button;
    UISliderProxy  Slider;
    UIImageProxy   Image;
    UIToggleProxy  Toggle;
    UIInputProxy   Input;

    UIProxy() : Button(nullptr), Slider(nullptr, nullptr),
                Image(nullptr), Toggle(nullptr, nullptr), Input(nullptr, nullptr) {}

    explicit UIProxy(ScriptContext* ctx, std::string key)
        : ctx_(ctx), instanceKey_(std::move(key)),
          Button(ctx),
          Slider(ctx, &instanceKey_),
          Image(ctx),
          Toggle(ctx, &instanceKey_),
          Input(ctx, &instanceKey_)
    {
        _pollEvents();
    }

    // Helper checks

    bool Exists() const {
        return ctx_ && ctx_->object && ctx_->object->hasUI &&
               ctx_->object->ui.type != UIElementType::None;
    }
    bool IsText()   const { return _isType(UIElementType::Text); }
    bool IsButton() const { return _isType(UIElementType::Button); }
    bool IsSlider() const { return _isType(UIElementType::Slider); }
    bool IsImage()  const {
        return ctx_ && ctx_->object && ctx_->object->hasUI &&
               (ctx_->object->ui.type == UIElementType::Image ||
                ctx_->object->ui.type == UIElementType::Sprite2D);
    }
    bool IsToggle() const { return false; }  // not yet in engine
    bool IsInput()  const { return false; }  // not yet in engine

    // Direct property proxies

    // Text / Label
    struct TextProp {
        UIProxy* p;
        TextProp& operator=(const std::string& v) {
            if (p->_hasUI()) p->ctx_->object->ui.label = v;
            else detail::uiWarnOnce(p->ctx_, "Text");
            return *this;
        }
        TextProp& operator=(const char* v) { return *this = std::string(v ? v : ""); }
        operator std::string() const {
            return p->_hasUI() ? p->ctx_->object->ui.label : std::string{};
        }
    };
    TextProp Text{ this };

    // FontSize
    struct FontSizeProp {
        UIProxy* p;
        FontSizeProp& operator=(float v) {
            if (p->_hasUI()) p->ctx_->object->ui.fontSize = std::max(0.0f, v);
            else detail::uiWarnOnce(p->ctx_, "FontSize");
            return *this;
        }
        operator float() const {
            return p->_hasUI() ? p->ctx_->object->ui.fontSize : 0.0f;
        }
    };
    FontSizeProp FontSize{ this };

    // Position
    struct PositionProp {
        UIProxy* p;
        PositionProp& operator=(const glm::vec2& v) {
            if (p->_hasUI()) p->ctx_->object->ui.position = v;
            else detail::uiWarnOnce(p->ctx_, "Position");
            return *this;
        }
        operator glm::vec2() const {
            return p->_hasUI() ? p->ctx_->object->ui.position : glm::vec2(0.0f);
        }
    };
    PositionProp Position{ this };

    // Size
    struct SizeProp {
        UIProxy* p;
        SizeProp& operator=(const glm::vec2& v) {
            if (p->_hasUI()) p->ctx_->object->ui.size = glm::max(v, glm::vec2(0.01f));
            else detail::uiWarnOnce(p->ctx_, "Size");
            return *this;
        }
        operator glm::vec2() const {
            return p->_hasUI() ? p->ctx_->object->ui.size : glm::vec2(0.0f);
        }
    };
    SizeProp Size{ this };

    // Anchor
    struct AnchorProp {
        UIProxy* p;
        AnchorProp& operator=(int v) {
            if (p->_hasUI())
                p->ctx_->object->ui.anchor = static_cast<UIAnchor>(
                    std::clamp(v, 0, static_cast<int>(UIAnchor::BottomRight)));
            else detail::uiWarnOnce(p->ctx_, "Anchor");
            return *this;
        }
        operator int() const {
            return p->_hasUI() ? static_cast<int>(p->ctx_->object->ui.anchor) : 0;
        }
    };
    AnchorProp Anchor{ this };

    // Rotation
    struct RotationProp {
        UIProxy* p;
        RotationProp& operator=(float v) {
            if (p->_hasUI()) p->ctx_->object->ui.rotation = v;
            else detail::uiWarnOnce(p->ctx_, "Rotation");
            return *this;
        }
        operator float() const {
            return p->_hasUI() ? p->ctx_->object->ui.rotation : 0.0f;
        }
    };
    RotationProp Rotation{ this };

    // Scale (maps to textScale for text, or as a size multiplier for others)
    struct ScaleProp {
        UIProxy* p;
        ScaleProp& operator=(const glm::vec2& v) {
            if (p->_hasUI()) {
                // Apply as uniform scale on size, or textScale for text elements
                if (p->ctx_->object->ui.type == UIElementType::Text) {
                    p->ctx_->object->ui.textScale = std::max(0.01f, (v.x + v.y) * 0.5f);
                } else {
                    p->ctx_->object->ui.size = glm::max(
                        p->ctx_->object->ui.size * v, glm::vec2(0.01f));
                }
            } else {
                detail::uiWarnOnce(p->ctx_, "Scale");
            }
            return *this;
        }
        ScaleProp& operator=(float v) { return *this = glm::vec2(v, v); }
        operator glm::vec2() const {
            if (!p->_hasUI()) return glm::vec2(1.0f);
            if (p->ctx_->object->ui.type == UIElementType::Text)
                return glm::vec2(p->ctx_->object->ui.textScale);
            return glm::vec2(1.0f);
        }
    };
    ScaleProp Scale{ this };

    // Color (main tint)
    struct ColorProp {
        UIProxy* p;
        ColorProp& operator=(const glm::vec4& v) {
            if (p->_hasUI()) p->ctx_->object->ui.color = v;
            else detail::uiWarnOnce(p->ctx_, "Color");
            return *this;
        }
        operator glm::vec4() const {
            return p->_hasUI() ? p->ctx_->object->ui.color : glm::vec4(1.0f);
        }
    };
    ColorProp Color{ this };

    // TextColor
    struct TextColorProp {
        UIProxy* p;
        TextColorProp& operator=(const glm::vec4& v) {
            if (p->_hasUI()) p->ctx_->object->ui.textColor = v;
            else detail::uiWarnOnce(p->ctx_, "TextColor");
            return *this;
        }
        operator glm::vec4() const {
            return p->_hasUI() ? p->ctx_->object->ui.textColor : glm::vec4(0.0f);
        }
    };
    TextColorProp TextColor{ this };

    // BackgroundColor
    struct BackgroundColorProp {
        UIProxy* p;
        BackgroundColorProp& operator=(const glm::vec4& v) {
            if (p->_hasUI()) p->ctx_->object->ui.backgroundColor = v;
            else detail::uiWarnOnce(p->ctx_, "BackgroundColor");
            return *this;
        }
        operator glm::vec4() const {
            return p->_hasUI() ? p->ctx_->object->ui.backgroundColor : glm::vec4(0.0f);
        }
    };
    BackgroundColorProp BackgroundColor{ this };

    // Tint (alias for Color, also used as fillColor override)
    struct TintProp {
        UIProxy* p;
        TintProp& operator=(const glm::vec4& v) {
            if (p->_hasUI()) p->ctx_->object->ui.fillColor = v;
            else detail::uiWarnOnce(p->ctx_, "Tint");
            return *this;
        }
        operator glm::vec4() const {
            return p->_hasUI() ? p->ctx_->object->ui.fillColor : glm::vec4(0.0f);
        }
    };
    TintProp Tint{ this };

    // Alpha
    struct AlphaProp {
        UIProxy* p;
        AlphaProp& operator=(float v) {
            if (p->_hasUI()) p->ctx_->object->ui.color.a = std::clamp(v, 0.0f, 1.0f);
            else detail::uiWarnOnce(p->ctx_, "Alpha");
            return *this;
        }
        operator float() const {
            return p->_hasUI() ? p->ctx_->object->ui.color.a : 1.0f;
        }
    };
    AlphaProp Alpha{ this };

    // Typewriter reveal (Text elements).
    //
    // Set Text to the WHOLE message once, then drive TypeReveal from 0 upwards.
    // Wrapping and centring are measured from the full string, so the line never
    // shifts while it types itself in - the glyphs simply arrive in place.
    // Negative disables the reveal and the label draws in full.
    struct TypeRevealProp {
        UIProxy* p;
        TypeRevealProp& operator=(float v) {
            if (p->_hasUI()) p->ctx_->object->ui.textRevealChars = v;
            else detail::uiWarnOnce(p->ctx_, "TypeReveal");
            return *this;
        }
        operator float() const {
            return p->_hasUI() ? p->ctx_->object->ui.textRevealChars : -1.0f;
        }
    };
    TypeRevealProp TypeReveal{ this };

    // Size a glyph is born at, as a multiple of the settled size (1.4 = 40% big).
    struct TypePopScaleProp {
        UIProxy* p;
        TypePopScaleProp& operator=(float v) {
            if (p->_hasUI()) p->ctx_->object->ui.textRevealPopScale = std::max(0.01f, v);
            else detail::uiWarnOnce(p->ctx_, "TypePopScale");
            return *this;
        }
        operator float() const {
            return p->_hasUI() ? p->ctx_->object->ui.textRevealPopScale : 1.0f;
        }
    };
    TypePopScaleProp TypePopScale{ this };

    // How many glyphs a letter takes to settle from born to normal. Larger means
    // more letters are mid-fade at once, which is what reads as dreamy.
    struct TypeSoftnessProp {
        UIProxy* p;
        TypeSoftnessProp& operator=(float v) {
            if (p->_hasUI()) p->ctx_->object->ui.textRevealSoftness = std::max(0.0001f, v);
            else detail::uiWarnOnce(p->ctx_, "TypeSoftness");
            return *this;
        }
        operator float() const {
            return p->_hasUI() ? p->ctx_->object->ui.textRevealSoftness : 1.0f;
        }
    };
    TypeSoftnessProp TypeSoftness{ this };

    // Ambient per-glyph text effects (bit flags): 1 wave, 2 jitter, 4 bounce,
    // 8 orbit, 16 pulse. They compose with the reveal - effects move a glyph,
    // the reveal sizes and fades it.
    struct TextEffectsProp {
        UIProxy* p;
        TextEffectsProp& operator=(int v) {
            if (p->_hasUI()) p->ctx_->object->ui.textEffectFlags = v;
            else detail::uiWarnOnce(p->ctx_, "TextEffects");
            return *this;
        }
        operator int() const {
            return p->_hasUI() ? p->ctx_->object->ui.textEffectFlags : 0;
        }
    };
    TextEffectsProp TextEffects{ this };

    struct TextEffectSpeedProp {
        UIProxy* p;
        TextEffectSpeedProp& operator=(float v) {
            if (p->_hasUI()) p->ctx_->object->ui.textEffectSpeed = v;
            else detail::uiWarnOnce(p->ctx_, "TextEffectSpeed");
            return *this;
        }
        operator float() const {
            return p->_hasUI() ? p->ctx_->object->ui.textEffectSpeed : 1.0f;
        }
    };
    TextEffectSpeedProp TextEffectSpeed{ this };

    struct TextEffectIntensityProp {
        UIProxy* p;
        TextEffectIntensityProp& operator=(float v) {
            if (p->_hasUI()) p->ctx_->object->ui.textEffectIntensity = v;
            else detail::uiWarnOnce(p->ctx_, "TextEffectIntensity");
            return *this;
        }
        operator float() const {
            return p->_hasUI() ? p->ctx_->object->ui.textEffectIntensity : 1.0f;
        }
    };
    TextEffectIntensityProp TextEffectIntensity{ this };

    // Visible (maps to enabled on the SceneObject)
    struct VisibleProp {
        UIProxy* p;
        VisibleProp& operator=(bool v) {
            if (p->ctx_ && p->ctx_->object) p->ctx_->object->enabled = v;
            else detail::uiWarnOnce(p->ctx_, "Visible");
            return *this;
        }
        operator bool() const {
            return p->ctx_ && p->ctx_->object && p->ctx_->object->enabled;
        }
    };
    VisibleProp Visible{ this };

    // Enabled (same as Visible for UI)
    struct EnabledProp {
        UIProxy* p;
        EnabledProp& operator=(bool v) {
            if (p->ctx_ && p->ctx_->object) p->ctx_->object->enabled = v;
            else detail::uiWarnOnce(p->ctx_, "Enabled");
            return *this;
        }
        operator bool() const {
            return p->ctx_ && p->ctx_->object && p->ctx_->object->enabled;
        }
    };
    EnabledProp Enabled{ this };

    // Interactable
    struct InteractableProp {
        UIProxy* p;
        InteractableProp& operator=(bool v) {
            if (p->_hasUI()) p->ctx_->object->ui.interactable = v;
            else detail::uiWarnOnce(p->ctx_, "Interactable");
            return *this;
        }
        operator bool() const {
            return p->_hasUI() && p->ctx_->object->ui.interactable;
        }
    };
    InteractableProp Interactable{ this };

    // Layer
    struct LayerProp {
        UIProxy* p;
        LayerProp& operator=(int v) {
            if (p->ctx_ && p->ctx_->object) p->ctx_->object->layer = v;
            else detail::uiWarnOnce(p->ctx_, "Layer");
            return *this;
        }
        operator int() const {
            return p->ctx_ && p->ctx_->object ? p->ctx_->object->layer : 0;
        }
    };
    LayerProp Layer{ this };

    // SortingOrder
    struct SortingOrderProp {
        UIProxy* p;
        SortingOrderProp& operator=(int v) {
            if (p->_hasUI()) p->ctx_->object->ui.sortingOrder = v;
            else detail::uiWarnOnce(p->ctx_, "SortingOrder");
            return *this;
        }
        operator int() const {
            return p->_hasUI() ? p->ctx_->object->ui.sortingOrder : 0;
        }
    };
    SortingOrderProp SortingOrder{ this };

    // Order (alias for SortingOrder, shorter syntax)
    struct OrderProp {
        UIProxy* p;
        OrderProp& operator=(int v) {
            if (p->_hasUI()) p->ctx_->object->ui.sortingOrder = v;
            else detail::uiWarnOnce(p->ctx_, "Order");
            return *this;
        }
        operator int() const {
            return p->_hasUI() ? p->ctx_->object->ui.sortingOrder : 0;
        }
    };
    OrderProp Order{ this };

    // SetFront / SetBack helpers
    void SetFront() {
        if (_hasUI()) ctx_->object->ui.sortingOrder = 1000;
        else detail::uiWarnOnce(ctx_, "SetFront");
    }
    void SetBack() {
        if (_hasUI()) ctx_->object->ui.sortingOrder = -1000;
        else detail::uiWarnOnce(ctx_, "SetBack");
    }

    // Pivot (maps to pseudo3D pivot or a general concept - for now we use pseudo3DPivot)
    struct PivotProp {
        UIProxy* p;
        PivotProp& operator=(const glm::vec2& v) {
            if (p->_hasUI()) p->ctx_->object->ui.pseudo3DPivot = glm::clamp(v, glm::vec2(0.0f), glm::vec2(1.0f));
            else detail::uiWarnOnce(p->ctx_, "Pivot");
            return *this;
        }
        operator glm::vec2() const {
            return p->_hasUI() ? p->ctx_->object->ui.pseudo3DPivot : glm::vec2(0.5f);
        }
    };
    PivotProp Pivot{ this };

    // Event registration

    void OnClick(std::function<void()> fn) {
        detail::uiCallbackStore()[instanceKey_].onClick = std::move(fn);
    }
    void OnHover(std::function<void()> fn) {
        detail::uiCallbackStore()[instanceKey_].onHover = std::move(fn);
    }
    void OnUnhover(std::function<void()> fn) {
        detail::uiCallbackStore()[instanceKey_].onUnhover = std::move(fn);
    }
    void OnPress(std::function<void()> fn) {
        detail::uiCallbackStore()[instanceKey_].onPress = std::move(fn);
    }
    void OnRelease(std::function<void()> fn) {
        detail::uiCallbackStore()[instanceKey_].onRelease = std::move(fn);
    }
    void OnUnclick(std::function<void()> fn) {
        detail::uiCallbackStore()[instanceKey_].onUnclick = std::move(fn);
    }

private:
    bool _hasUI() const {
        return ctx_ && ctx_->object && ctx_->object->hasUI &&
               ctx_->object->ui.type != UIElementType::None;
    }

    bool _isType(UIElementType t) const {
        return ctx_ && ctx_->object && ctx_->object->hasUI &&
               ctx_->object->ui.type == t;
    }

    void _pollEvents() {
        if (instanceKey_.empty()) return;
        auto cbIt = detail::uiCallbackStore().find(instanceKey_);
        if (cbIt == detail::uiCallbackStore().end()) return;
        auto& cbs = cbIt->second;

        detail::UIEventState& state = detail::uiEventStateStore()[instanceKey_];

        // Click
        if (cbs.onClick && ctx_ && ctx_->IsUIButtonPressed()) {
            cbs.onClick();
        }

        // Hover / Unhover
        const bool nowHovered = ctx_ ? ctx_->IsUIHovered() : false;
        if (cbs.onHover && nowHovered && !state.wasHovered) {
            cbs.onHover();
        }
        if (cbs.onUnhover && !nowHovered && state.wasHovered) {
            cbs.onUnhover();
        }
        state.wasHovered = nowHovered;

        // Press / Release
        const bool nowActive = ctx_ ? ctx_->IsUIActive() : false;
        if (cbs.onPress && nowActive && !state.wasActive) {
            cbs.onPress();
        }
        if (cbs.onRelease && !nowActive && state.wasActive) {
            cbs.onRelease();
        }
        state.wasActive = nowActive;

        // Slider value changed
        if (cbs.onValueChanged && ctx_ && ctx_->object &&
            ctx_->object->hasUI && ctx_->object->ui.type == UIElementType::Slider) {
            const float nowVal = ctx_->object->ui.sliderValue;
            if (state.lastSliderValue > -1e37f &&
                std::abs(nowVal - state.lastSliderValue) > 1e-6f) {
                cbs.onValueChanged(nowVal);
            }
            state.lastSliderValue = nowVal;
        }
    }
};

// Make UIProxy for a given ScriptContext

inline UIProxy MakeUIProxy(ScriptContext& ctx, const std::string& instanceKey) {
    return UIProxy(&ctx, instanceKey);
}

} // namespace ModuCPP
