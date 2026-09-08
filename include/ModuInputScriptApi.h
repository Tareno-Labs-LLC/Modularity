#pragma once

#include "ModuCPPScriptApi.h"

#include <GLFW/glfw3.h>
#include <cmath>
#include <cstring>

namespace ModuCPP {

namespace detail {
inline ImGuiKey glfwToImGuiKey(int key) {
    switch (key) {
        case GLFW_KEY_W: return ImGuiKey_W;
        case GLFW_KEY_A: return ImGuiKey_A;
        case GLFW_KEY_S: return ImGuiKey_S;
        case GLFW_KEY_D: return ImGuiKey_D;
        case GLFW_KEY_E: return ImGuiKey_E;
        case GLFW_KEY_UP: return ImGuiKey_UpArrow;
        case GLFW_KEY_DOWN: return ImGuiKey_DownArrow;
        case GLFW_KEY_LEFT: return ImGuiKey_LeftArrow;
        case GLFW_KEY_RIGHT: return ImGuiKey_RightArrow;
        case GLFW_KEY_LEFT_SHIFT: return ImGuiKey_LeftShift;
        case GLFW_KEY_RIGHT_SHIFT: return ImGuiKey_RightShift;
        case GLFW_KEY_SPACE: return ImGuiKey_Space;
        case GLFW_KEY_ENTER: return ImGuiKey_Enter;
        case GLFW_KEY_KP_ENTER: return ImGuiKey_KeypadEnter;
        default: return ImGuiKey_None;
    }
}
} // namespace detail

constexpr int KEY_W = GLFW_KEY_W;
constexpr int KEY_A = GLFW_KEY_A;
constexpr int KEY_S = GLFW_KEY_S;
constexpr int KEY_D = GLFW_KEY_D;
constexpr int KEY_E = GLFW_KEY_E;
constexpr int KEY_UP = GLFW_KEY_UP;
constexpr int KEY_DOWN = GLFW_KEY_DOWN;
constexpr int KEY_LEFT = GLFW_KEY_LEFT;
constexpr int KEY_RIGHT = GLFW_KEY_RIGHT;
constexpr int KEY_SHIFT_LEFT = GLFW_KEY_LEFT_SHIFT;
constexpr int KEY_SHIFT_RIGHT = GLFW_KEY_RIGHT_SHIFT;
constexpr int KEY_SPACE = GLFW_KEY_SPACE;
constexpr int KEY_ENTER = GLFW_KEY_ENTER;
constexpr int KEY_KP_ENTER = GLFW_KEY_KP_ENTER;

inline bool KeyDown(ScriptContext& ctx, int key) {
    return ctx.IsKeyDown(key, detail::glfwToImGuiKey(key));
}

inline bool KeyDown(int key) {
    if (ScriptContext* scriptCtx = ctxPtr()) return KeyDown(*scriptCtx, key);
    return false;
}

inline bool KeyPressed(ScriptContext& ctx, int key) {
    return ctx.IsKeyPressed(key, detail::glfwToImGuiKey(key));
}

inline bool KeyPressed(int key) {
    if (ScriptContext* scriptCtx = ctxPtr()) return KeyPressed(*scriptCtx, key);
    return false;
}

struct InputFacade {
    vec2 WASD() const {
        vec2 move(0.0f);
        if (KeyDown(KEY_W)) move.y += 1.0f;
        if (KeyDown(KEY_S)) move.y -= 1.0f;
        if (KeyDown(KEY_D)) move.x += 1.0f;
        if (KeyDown(KEY_A)) move.x -= 1.0f;
        return move;
    }

    vec2 WASDNormalized() const {
        vec2 move = WASD();
        const float len = glm::length(move);
        if (len > 1e-4f) {
            move /= len;
        }
        return move;
    }

    bool sprint() const {
        return KeyDown(KEY_SHIFT_LEFT) || KeyDown(KEY_SHIFT_RIGHT);
    }

    bool jump() const {
        return KeyDown(KEY_SPACE);
    }
};

inline const InputFacade input{};

// Touch / pointer input. On Android this surfaces the live multi-touch set from
// the NativeActivity runtime; on desktop the left mouse button is reported as a
// single touch so the exact same Touch code runs when you test in the editor or
// the desktop player. Coordinates are surface/framebuffer pixels, origin top-left
// (normalize against the viewport size yourself if you want 0..1).
struct TouchFacade {
    // How many fingers are down right now (desktop: 1 while LMB is held, else 0).
    int Count() const {
        if (ScriptContext* c = ctxPtr()) return c->GetTouchCount();
        return 0;
    }

    // Is anything touching the screen at all?
    bool IsActive() const {
        if (ScriptContext* c = ctxPtr()) return c->IsTouchActive();
        return false;
    }

    // Position of the index-th active finger. Returns (0,0) if there's no such
    // finger this frame, so guard with Count() when it matters.
    vec2 Position(int index = 0) const {
        vec2 p(0.0f);
        if (ScriptContext* c = ctxPtr()) {
            float x = 0.0f, y = 0.0f;
            if (c->GetTouch(index, x, y)) { p.x = x; p.y = y; }
        }
        return p;
    }

    // Primary pointer (oldest finger / the mouse). `out` is left untouched when
    // nothing is down, so you can keep a last-known position across taps.
    bool Primary(vec2& out) const {
        if (ScriptContext* c = ctxPtr()) {
            float x = 0.0f, y = 0.0f;
            if (c->GetPrimaryTouch(x, y)) { out.x = x; out.y = y; return true; }
        }
        return false;
    }
    vec2 Primary() const { vec2 p(0.0f); Primary(p); return p; }

    // true only on the frame the primary touch goes down / lifts. Tapped() is
    // an alias for Began() since a tap is just a press edge.
    bool Began() const { return edge(true); }
    bool Ended() const { return edge(false); }
    bool Tapped() const { return Began(); }

    // Primary-pointer movement since last frame (zero when inactive or on the
    // frame it began). Handy for drag-to-look / swipe gestures.
    vec2 Drag() const {
        DragState& s = dragState();
        const int frame = ImGui::GetFrameCount();
        if (s.frame != frame) {
            vec2 cur(0.0f);
            const bool active = Primary(cur);
            vec2 delta(0.0f);
            if (active && s.hadPrev) {
                delta.x = cur.x - s.prevX;
                delta.y = cur.y - s.prevY;
            }
            s.prevX = cur.x;
            s.prevY = cur.y;
            s.hadPrev = active;
            s.lastDelta = delta;
            s.frame = frame;
        }
        return s.lastDelta;
    }

private:
    // Per-frame edge/drag bookkeeping, keyed by ImGui frame so multiple calls in
    // one tick agree. Single-pointer (primary) only; that covers taps/swipes.
    struct EdgeState { int frame = -1; bool prevActive = false; bool down = false; bool up = false; };
    struct DragState { int frame = -1; bool hadPrev = false; float prevX = 0.0f; float prevY = 0.0f; vec2 lastDelta = vec2(0.0f); };
    static EdgeState& edgeState() { static EdgeState s; return s; }
    static DragState& dragState() { static DragState s; return s; }

    bool edge(bool wantDown) const {
        EdgeState& s = edgeState();
        const int frame = ImGui::GetFrameCount();
        if (s.frame != frame) {
            const bool active = IsActive();
            s.down = active && !s.prevActive;
            s.up   = !active && s.prevActive;
            s.prevActive = active;
            s.frame = frame;
        }
        return wantDown ? s.down : s.up;
    }
};

inline const TouchFacade touch{};

inline bool IsRuntimeKeyDown(int glfwKey, ImGuiKey imguiKey) {
    if (ctxPtr()) {
        return KeyDown(glfwKey);
    }
    if (ImGui::IsKeyDown(imguiKey)) return true;
    GLFWwindow* window = glfwGetCurrentContext();
    if (!window) return false;
    return glfwGetKey(window, glfwKey) == GLFW_PRESS;
}

namespace Key {
constexpr int W = GLFW_KEY_W;
constexpr int A = GLFW_KEY_A;
constexpr int S = GLFW_KEY_S;
constexpr int D = GLFW_KEY_D;
constexpr int E = GLFW_KEY_E;
constexpr int UpArrow = GLFW_KEY_UP;
constexpr int DownArrow = GLFW_KEY_DOWN;
constexpr int LeftArrow = GLFW_KEY_LEFT;
constexpr int RightArrow = GLFW_KEY_RIGHT;
// Short aliases - equivalent to the *Arrow variants above.
constexpr int Up = GLFW_KEY_UP;
constexpr int Down = GLFW_KEY_DOWN;
constexpr int Left = GLFW_KEY_LEFT;
constexpr int Right = GLFW_KEY_RIGHT;
constexpr int LeftShift = GLFW_KEY_LEFT_SHIFT;
constexpr int RightShift = GLFW_KEY_RIGHT_SHIFT;
constexpr int Space = GLFW_KEY_SPACE;
constexpr int Enter = GLFW_KEY_ENTER;
constexpr int KeypadEnter = GLFW_KEY_KP_ENTER;
} // namespace Key

inline bool IsKeyDown(int key) { return KeyDown(key); }
inline bool IsKeyDown(int keyA, int keyB) { return KeyDown(keyA) || KeyDown(keyB); }
inline bool IsKeyPressed(int key) { return KeyPressed(key); }

inline bool KeyReleased(int key) {
    const ImGuiKey imguiKey = detail::glfwToImGuiKey(key);
    if (imguiKey != ImGuiKey_None && ImGui::IsKeyReleased(imguiKey)) return true;
    return false;
}

inline bool IsKeyReleased(int key) { return KeyReleased(key); }

namespace InputActions {

struct ActionBinding {
    std::string name;
    std::vector<int> keys;
};

inline std::unordered_map<std::string, std::vector<int>>& Registry() {
    static std::unordered_map<std::string, std::vector<int>> r = {
        { "Jump",     { GLFW_KEY_SPACE } },
        { "Sprint",   { GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT } },
        { "Interact", { GLFW_KEY_E } },
        { "Throw",    { GLFW_KEY_F } },
        { "Crouch",   { GLFW_KEY_C } },
        { "Submit",   { GLFW_KEY_ENTER, GLFW_KEY_KP_ENTER } },
        { "Cancel",   { GLFW_KEY_ESCAPE } },
        { "Up",       { GLFW_KEY_W, GLFW_KEY_UP } },
        { "Down",     { GLFW_KEY_S, GLFW_KEY_DOWN } },
        { "Left",     { GLFW_KEY_A, GLFW_KEY_LEFT } },
        { "Right",    { GLFW_KEY_D, GLFW_KEY_RIGHT } },
    };
    return r;
}

inline int lookupKeyName(const std::string& token) {
    static const std::unordered_map<std::string, int> kNameToKey = {
        { "Space", GLFW_KEY_SPACE }, { "Enter", GLFW_KEY_ENTER },
        { "KeypadEnter", GLFW_KEY_KP_ENTER }, { "Escape", GLFW_KEY_ESCAPE },
        { "LeftShift", GLFW_KEY_LEFT_SHIFT }, { "RightShift", GLFW_KEY_RIGHT_SHIFT },
        { "LeftCtrl", GLFW_KEY_LEFT_CONTROL }, { "RightCtrl", GLFW_KEY_RIGHT_CONTROL },
        { "Up", GLFW_KEY_UP }, { "Down", GLFW_KEY_DOWN },
        { "Left", GLFW_KEY_LEFT }, { "Right", GLFW_KEY_RIGHT },
        { "Tab", GLFW_KEY_TAB }, { "Backspace", GLFW_KEY_BACKSPACE },
        { "W", GLFW_KEY_W }, { "A", GLFW_KEY_A }, { "S", GLFW_KEY_S }, { "D", GLFW_KEY_D },
        { "E", GLFW_KEY_E }, { "F", GLFW_KEY_F }, { "Q", GLFW_KEY_Q },
        { "R", GLFW_KEY_R }, { "T", GLFW_KEY_T }, { "C", GLFW_KEY_C },
        { "K", GLFW_KEY_K },
    };
    auto it = kNameToKey.find(token);
    if (it != kNameToKey.end()) return it->second;
    if (token.size() == 1) {
        char c = token[0];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return GLFW_KEY_A + (c - 'A');
        if (c >= '0' && c <= '9') return GLFW_KEY_0 + (c - '0');
    }
    return GLFW_KEY_UNKNOWN;
}

inline bool& loadedFlag() {
    static bool loaded = false;
    return loaded;
}

inline void parseFile(const std::string& contents) {
    auto& registry = Registry();
    std::string line;
    line.reserve(128);
    size_t i = 0;
    while (i <= contents.size()) {
        const char c = (i < contents.size()) ? contents[i] : '\n';
        if (c == '\n' || c == '\r') {
            if (!line.empty()) {
                size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string name = line.substr(0, eq);
                    std::string rhs  = line.substr(eq + 1);
                    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
                    size_t s = 0;
                    while (s < name.size() && (name[s] == ' ' || name[s] == '\t')) ++s;
                    name = name.substr(s);
                    if (!name.empty() && name[0] != '#') {
                        std::vector<int> keys;
                        size_t p = 0;
                        while (p <= rhs.size()) {
                            if (p == rhs.size() || rhs[p] == ',') {
                                std::string tok = rhs.substr(0, p);
                                while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
                                size_t ts = 0;
                                while (ts < tok.size() && (tok[ts] == ' ' || tok[ts] == '\t')) ++ts;
                                tok = tok.substr(ts);
                                if (!tok.empty()) {
                                    const int key = lookupKeyName(tok);
                                    if (key != GLFW_KEY_UNKNOWN) keys.push_back(key);
                                }
                                // p == rhs.size() means that was the final token, and
                                // substr(size + 1) throws. This stayed dormant only
                                // because no project had shipped an input_actions.modu
                                // yet; the first one crashed play mode on its first line.
                                if (p + 1 > rhs.size()) break;
                                rhs = rhs.substr(p + 1);
                                p = 0;
                                if (rhs.empty()) break;
                                continue;
                            }
                            ++p;
                        }
                        if (!keys.empty()) registry[name] = std::move(keys);
                    }
                }
                line.clear();
            }
        } else {
            line.push_back(c);
        }
        ++i;
    }
}

inline void ensureLoaded() {
    if (loadedFlag()) return;
    loadedFlag() = true;
    if (ScriptContext* c = ctxPtr()) {
        const std::string text = c->ReadFileText("input_actions.modu");
        if (!text.empty()) parseFile(text);
    }
}

inline const std::vector<int>* find(const std::string& name) {
    ensureLoaded();
    auto& r = Registry();
    auto it = r.find(name);
    if (it == r.end()) return nullptr;
    return &it->second;
}

} // namespace InputActions

// Gamepad, straight off GLFW's mapped-controller API (Xbox layout naming; GLFW's built-in
// SDL mapping DB translates DualShock/Switch/etc. onto it). Deliberately NOT routed
// through ImGui: ImGuiConfigFlags_NavEnableGamepad is off so a connected pad can never
// drive editor windows while you are play-testing, and gameplay still reads it here.
namespace Gamepad {

constexpr int ButtonA           = GLFW_GAMEPAD_BUTTON_A;
constexpr int ButtonB           = GLFW_GAMEPAD_BUTTON_B;
constexpr int ButtonX           = GLFW_GAMEPAD_BUTTON_X;
constexpr int ButtonY           = GLFW_GAMEPAD_BUTTON_Y;
constexpr int ButtonStart       = GLFW_GAMEPAD_BUTTON_START;
constexpr int ButtonBack        = GLFW_GAMEPAD_BUTTON_BACK;
constexpr int ButtonLeftBumper  = GLFW_GAMEPAD_BUTTON_LEFT_BUMPER;
constexpr int ButtonRightBumper = GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER;
constexpr int ButtonDpadUp      = GLFW_GAMEPAD_BUTTON_DPAD_UP;
constexpr int ButtonDpadDown    = GLFW_GAMEPAD_BUTTON_DPAD_DOWN;
constexpr int ButtonDpadLeft    = GLFW_GAMEPAD_BUTTON_DPAD_LEFT;
constexpr int ButtonDpadRight   = GLFW_GAMEPAD_BUTTON_DPAD_RIGHT;

constexpr int MaxPads = 4;

namespace detail {
// One glfwGetGamepadState poll per pad per frame, shared by every caller in that frame.
struct PadFrame {
    int frame = -1;
    bool connected = false;
    GLFWgamepadstate state{};
    unsigned char previousButtons[GLFW_GAMEPAD_BUTTON_LAST + 1] = {0};
    bool hasPrevious = false;
};

inline PadFrame& padFrame(int pad) {
    static PadFrame pads[MaxPads];
    if (pad < 0 || pad >= MaxPads) pad = 0;
    return pads[pad];
}

inline const PadFrame& poll(int pad) {
    PadFrame& p = padFrame(pad);
    const int frame = ImGui::GetFrameCount();
    if (p.frame == frame) return p;
    p.frame = frame;
    if (p.hasPrevious) {
        std::memcpy(p.previousButtons, p.state.buttons, sizeof(p.previousButtons));
    }
    GLFWgamepadstate next{};
    const int jid = GLFW_JOYSTICK_1 + ((pad < 0 || pad >= MaxPads) ? 0 : pad);
    p.connected = glfwGetGamepadState(jid, &next) == GLFW_TRUE;
    if (!p.connected) {
        next = GLFWgamepadstate{};
    }
    p.state = next;
    if (!p.hasPrevious) {
        std::memcpy(p.previousButtons, p.state.buttons, sizeof(p.previousButtons));
        p.hasPrevious = true;
    }
    return p;
}

inline float applyDeadzone(float value, float deadzone) {
    const float magnitude = std::fabs(value);
    if (magnitude <= deadzone) return 0.0f;
    // rescale so the axis still reaches 1.0 at the stick's edge instead of jumping.
    const float scaled = (magnitude - deadzone) / std::max(1e-4f, 1.0f - deadzone);
    return value < 0.0f ? -scaled : scaled;
}
} // namespace detail

inline bool Connected(int pad = 0) { return detail::poll(pad).connected; }

inline bool ButtonDown(int button, int pad = 0) {
    if (button < 0 || button > GLFW_GAMEPAD_BUTTON_LAST) return false;
    const detail::PadFrame& p = detail::poll(pad);
    return p.connected && p.state.buttons[button] == GLFW_PRESS;
}

// True only on the frame the button goes down.
inline bool ButtonPressed(int button, int pad = 0) {
    if (button < 0 || button > GLFW_GAMEPAD_BUTTON_LAST) return false;
    const detail::PadFrame& p = detail::poll(pad);
    if (!p.connected) return false;
    return p.state.buttons[button] == GLFW_PRESS && p.previousButtons[button] != GLFW_PRESS;
}

inline float Axis(int axis, float deadzone = 0.0f, int pad = 0) {
    if (axis < 0 || axis > GLFW_GAMEPAD_AXIS_LAST) return 0.0f;
    const detail::PadFrame& p = detail::poll(pad);
    if (!p.connected) return 0.0f;
    return detail::applyDeadzone(p.state.axes[axis], deadzone);
}

// Sticks use screen-style axes with +y UP, matching WASDMovement().
inline Vector2 LeftStick(float deadzone = 0.35f, int pad = 0) {
    return Vector2(Axis(GLFW_GAMEPAD_AXIS_LEFT_X, deadzone, pad),
                   -Axis(GLFW_GAMEPAD_AXIS_LEFT_Y, deadzone, pad));
}

inline Vector2 RightStick(float deadzone = 0.35f, int pad = 0) {
    return Vector2(Axis(GLFW_GAMEPAD_AXIS_RIGHT_X, deadzone, pad),
                   -Axis(GLFW_GAMEPAD_AXIS_RIGHT_Y, deadzone, pad));
}

inline Vector2 DPad(int pad = 0) {
    Vector2 dir(0.0f);
    if (ButtonDown(ButtonDpadRight, pad)) dir.x += 1.0f;
    if (ButtonDown(ButtonDpadLeft, pad))  dir.x -= 1.0f;
    if (ButtonDown(ButtonDpadUp, pad))    dir.y += 1.0f;
    if (ButtonDown(ButtonDpadDown, pad))  dir.y -= 1.0f;
    return dir;
}

// D-pad wins over the stick when both are pushed, so a menu never gets two inputs at once.
inline Vector2 Direction(float deadzone = 0.5f, int pad = 0) {
    const Vector2 dpad = DPad(pad);
    if (std::fabs(dpad.x) > 0.0f || std::fabs(dpad.y) > 0.0f) return dpad;
    return LeftStick(deadzone, pad);
}

} // namespace Gamepad

namespace Input {
inline bool IsKeyDown(int key) { return ::ModuCPP::KeyDown(key); }
inline bool IsKeyDown(int keyA, int keyB) { return ::ModuCPP::KeyDown(keyA) || ::ModuCPP::KeyDown(keyB); }
inline bool IsKeyPressed(int key) { return ::ModuCPP::KeyPressed(key); }
inline bool IsKeyReleased(int key) { return ::ModuCPP::KeyReleased(key); }

inline Vector2 WASDMovement() {
    Vector2 move(0.0f);
    if (KeyDown(GLFW_KEY_W) || KeyDown(GLFW_KEY_UP))    move.y += 1.0f;
    if (KeyDown(GLFW_KEY_S) || KeyDown(GLFW_KEY_DOWN))  move.y -= 1.0f;
    if (KeyDown(GLFW_KEY_D) || KeyDown(GLFW_KEY_RIGHT)) move.x += 1.0f;
    if (KeyDown(GLFW_KEY_A) || KeyDown(GLFW_KEY_LEFT))  move.x -= 1.0f;
    const float len = glm::length(move);
    if (len > 1e-4f) move /= len;
    return move;
}

inline bool ButtonHeld(const std::string& name) {
    if (const auto* keys = InputActions::find(name)) {
        for (int k : *keys) if (::ModuCPP::KeyDown(k)) return true;
        return false;
    }
    const int key = InputActions::lookupKeyName(name);
    return (key != GLFW_KEY_UNKNOWN) && ::ModuCPP::KeyDown(key);
}

inline bool ButtonDown(const std::string& name) {
    if (const auto* keys = InputActions::find(name)) {
        for (int k : *keys) if (::ModuCPP::KeyPressed(k)) return true;
        return false;
    }
    const int key = InputActions::lookupKeyName(name);
    return (key != GLFW_KEY_UNKNOWN) && ::ModuCPP::KeyPressed(key);
}

inline bool ButtonUp(const std::string& name) {
    if (const auto* keys = InputActions::find(name)) {
        for (int k : *keys) if (::ModuCPP::KeyReleased(k)) return true;
        return false;
    }
    const int key = InputActions::lookupKeyName(name);
    return (key != GLFW_KEY_UNKNOWN) && ::ModuCPP::KeyReleased(key);
}

// Menu/UI direction: WASD + arrows + d-pad + left stick, +y UP. Digital sources are
// returned at full deflection so a caller can threshold all of them the same way.
inline Vector2 NavigationVector(float deadzone = 0.5f, int pad = 0) {
    Vector2 dir = WASDMovement();
    if (std::fabs(dir.x) < 1e-4f && std::fabs(dir.y) < 1e-4f) {
        dir = Gamepad::Direction(deadzone, pad);
    }
    return dir;
}

// "Confirm" / "back" for menus: the named Submit/Cancel key bindings plus the pad's
// A / B (and Start/Back), so a controller drives a menu without any extra script code.
inline bool MenuSubmit(int pad = 0) {
    return ButtonDown("Submit") || Gamepad::ButtonPressed(Gamepad::ButtonA, pad) ||
           Gamepad::ButtonPressed(Gamepad::ButtonStart, pad);
}

inline bool MenuCancel(int pad = 0) {
    return ButtonDown("Cancel") || Gamepad::ButtonPressed(Gamepad::ButtonB, pad) ||
           Gamepad::ButtonPressed(Gamepad::ButtonBack, pad);
}

inline void SetBinding(const std::string& name, int key) {
    InputActions::Registry()[name] = { key };
}

inline void SetBinding(const std::string& name, std::initializer_list<int> keys) {
    InputActions::Registry()[name] = std::vector<int>(keys);
}

inline void ReloadBindings() {
    InputActions::loadedFlag() = false;
    InputActions::ensureLoaded();
}
} // namespace Input

inline bool IsSubmitDown() {
    return IsRuntimeKeyDown(GLFW_KEY_ENTER, ImGuiKey_Enter) ||
           IsRuntimeKeyDown(GLFW_KEY_KP_ENTER, ImGuiKey_KeypadEnter);
}

} // namespace ModuCPP
