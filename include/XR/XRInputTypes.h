#pragma once

// Shared vocabulary for XR input, used by both sides of the script boundary.
//
// The engine (src/XR/XRInput.*) and the script-facing facade
// (include/ModuInputXRScriptApi.h) must agree on exactly what "Trigger" or
// "Left" means, and a compiled script .so passes these across the ABI as plain
// integers. Keeping the enums in one header under include/ - which is copied
// into the script SDK - means there is a single definition rather than two lists
// that can drift apart silently.
//
// Deliberately free of any OpenXR, GL or engine dependency: this header is
// included by scripts, which must never see an OpenXR handle (section 48).
//
// Enumerator values are part of the script ABI. Append to the end; never
// renumber, and never reuse a removed value.

#include <cstddef>

namespace Modularity::XR {

// Which tracked device a query is about.
enum class XRDevice {
    Head = 0,
    Left = 1,
    Right = 2,
    Count = 3
};

// Which pose of a controller. Grip and Aim are genuinely different transforms
// and are never aliased (section 15): Grip follows the physical controller in
// the hand, Aim points where the controller is aiming, and on Touch controllers
// they differ by roughly 45 degrees.
enum class XRPoseKind {
    Grip = 0,  // hand/controller placement, held objects, controller models
    Aim = 1,   // ray pointers, weapons, UI targeting
    Count = 2
};

// Digital controls. Trigger and Grip appear here as well as in XRAxisKind: the
// button form is the thresholded version of the same physical control.
enum class XRButton {
    Trigger = 0,
    Grip = 1,
    PrimaryButton = 2,    // A on the right hand, X on the left
    SecondaryButton = 3,  // B on the right hand, Y on the left
    ThumbstickClick = 4,
    Menu = 5,             // left controller only on Meta Quest Touch
    Count = 6
};

// Analog controls with a single axis, reported 0..1.
enum class XRAxisKind {
    Trigger = 0,
    Grip = 1,
    Count = 2
};

// Analog controls with two axes, each reported -1..1.
enum class XRAxis2DKind {
    Thumbstick = 0,
    Count = 1
};

// How much the runtime currently vouches for a device's pose. Ordered by
// increasing confidence so a caller can write `>= Tracked`.
enum class XRTrackingState {
    NotTracked = 0,   // no pose at all
    Extrapolated = 1, // last known pose, no longer being observed
    Tracked = 2       // actively tracked right now
};

// Threshold at which an analog trigger or grip counts as pressed, with the
// release point deliberately lower so a control resting near the boundary does
// not chatter between pressed and released every frame.
//
// These live here rather than in the engine because a script that reads
// InputXR.Right.Axis("Trigger") and one that reads
// InputXR.Right.Button("Trigger") must agree on where the line is.
inline constexpr float kXRAnalogPressThreshold = 0.6f;
inline constexpr float kXRAnalogReleaseThreshold = 0.4f;

// Name lookups for the string-based script API.
//
// The string form (InputXR.Left.ButtonDown("Trigger")) is the API the user asked
// to keep, so these exist to make it cheap rather than to discourage it: each is
// a small linear scan over a fixed table with no allocation, and callers are
// expected to resolve once and cache the enum, which is exactly what the facade
// in ModuInputXRScriptApi.h does. Returns Count on an unknown name so the caller
// can report a typo instead of silently reading the wrong control.

namespace detail {

// Case-insensitive, and tolerant of the separators people actually type:
// "PrimaryButton", "primary_button" and "primary button" all match.
inline bool XRNameEquals(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca == '_' || ca == ' ' || ca == '-') { ++a; continue; }
        if (cb == '_' || cb == ' ' || cb == '-') { ++b; continue; }
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    // Trailing separators on either side still count as a match.
    while (*a == '_' || *a == ' ' || *a == '-') ++a;
    while (*b == '_' || *b == ' ' || *b == '-') ++b;
    return *a == '\0' && *b == '\0';
}

struct XRNamedButton { const char* name; XRButton value; };
struct XRNamedAxis { const char* name; XRAxisKind value; };
struct XRNamedAxis2D { const char* name; XRAxis2DKind value; };

// Aliases are intentional: people coming from other engines type "Squeeze" for
// grip and "PrimaryAxis" for the thumbstick, and failing those is a papercut
// with no upside.
inline constexpr XRNamedButton kXRButtonNames[] = {
    { "Trigger",         XRButton::Trigger },
    { "Grip",            XRButton::Grip },
    { "Squeeze",         XRButton::Grip },
    { "PrimaryButton",   XRButton::PrimaryButton },
    { "Primary",         XRButton::PrimaryButton },
    { "A",               XRButton::PrimaryButton },
    { "X",               XRButton::PrimaryButton },
    { "SecondaryButton", XRButton::SecondaryButton },
    { "Secondary",       XRButton::SecondaryButton },
    { "B",               XRButton::SecondaryButton },
    { "Y",               XRButton::SecondaryButton },
    { "ThumbstickClick", XRButton::ThumbstickClick },
    { "StickClick",      XRButton::ThumbstickClick },
    { "Menu",            XRButton::Menu },
    { "Start",           XRButton::Menu },
};

inline constexpr XRNamedAxis kXRAxisNames[] = {
    { "Trigger", XRAxisKind::Trigger },
    { "Grip",    XRAxisKind::Grip },
    { "Squeeze", XRAxisKind::Grip },
};

inline constexpr XRNamedAxis2D kXRAxis2DNames[] = {
    { "Thumbstick",  XRAxis2DKind::Thumbstick },
    { "Stick",       XRAxis2DKind::Thumbstick },
    { "PrimaryAxis", XRAxis2DKind::Thumbstick },
    { "Joystick",    XRAxis2DKind::Thumbstick },
};

} // namespace detail

inline XRButton XRButtonFromName(const char* name) {
    for (const detail::XRNamedButton& entry : detail::kXRButtonNames) {
        if (detail::XRNameEquals(entry.name, name)) return entry.value;
    }
    return XRButton::Count;
}

inline XRAxisKind XRAxisFromName(const char* name) {
    for (const detail::XRNamedAxis& entry : detail::kXRAxisNames) {
        if (detail::XRNameEquals(entry.name, name)) return entry.value;
    }
    return XRAxisKind::Count;
}

inline XRAxis2DKind XRAxis2DFromName(const char* name) {
    for (const detail::XRNamedAxis2D& entry : detail::kXRAxis2DNames) {
        if (detail::XRNameEquals(entry.name, name)) return entry.value;
    }
    return XRAxis2DKind::Count;
}

// Canonical display names, for inspectors and diagnostics. Defined inline so a
// script binary that uses them needs nothing from the engine at link time - the
// script SDK ships headers, not a library to link against.
inline const char* ToString(XRDevice device) {
    switch (device) {
        case XRDevice::Head:  return "Head";
        case XRDevice::Left:  return "Left";
        case XRDevice::Right: return "Right";
        default:              return "Unknown";
    }
}

inline const char* ToString(XRButton button) {
    switch (button) {
        case XRButton::Trigger:         return "Trigger";
        case XRButton::Grip:            return "Grip";
        case XRButton::PrimaryButton:   return "Primary Button";
        case XRButton::SecondaryButton: return "Secondary Button";
        case XRButton::ThumbstickClick: return "Thumbstick Click";
        case XRButton::Menu:            return "Menu";
        default:                        return "Unknown";
    }
}

inline const char* ToString(XRAxisKind axis) {
    switch (axis) {
        case XRAxisKind::Trigger: return "Trigger";
        case XRAxisKind::Grip:    return "Grip";
        default:                  return "Unknown";
    }
}

inline const char* ToString(XRAxis2DKind axis) {
    switch (axis) {
        case XRAxis2DKind::Thumbstick: return "Thumbstick";
        default:                       return "Unknown";
    }
}

inline const char* ToString(XRPoseKind pose) {
    switch (pose) {
        case XRPoseKind::Grip: return "Grip Pose";
        case XRPoseKind::Aim:  return "Aim Pose";
        default:               return "Unknown";
    }
}

inline const char* ToString(XRTrackingState state) {
    switch (state) {
        case XRTrackingState::NotTracked:   return "Not Tracked";
        case XRTrackingState::Extrapolated: return "Extrapolated";
        case XRTrackingState::Tracked:      return "Tracked";
        default:                            return "Unknown";
    }
}

} // namespace Modularity::XR
